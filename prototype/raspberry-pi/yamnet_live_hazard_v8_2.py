#!/usr/bin/env python3
"""Audio-Master V8.2: ESP32 Mini framed PCM -> Pi YAMNet -> Korvo-2 UART hazard.

Mini: Phase-2 firmware, AMS1 packet protocol, 921600bps over USB-UART.
Korvo-2: already tested V8.0 firmware, 115200bps over a separate USB-UART.

This is a prototype with heuristic thresholds, not a safety-certified detector.
"""
import argparse
import csv
import os
import struct
import threading
import time
import wave
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

MAGIC = b"AMS1"
HEADER = struct.Struct("<4sBHI")
MAX_PACKET_PAYLOAD = 4096  # Mini's PCM payload is normally 1024 bytes.
PKT_EVENT_START = 1
PKT_FEATURE = 2
PKT_PCM = 3
PKT_EVENT_END = 4
PKT_HELLO = 5

SAMPLE_RATE = 16000
WINDOW_SAMPLES = 15360     # 960ms of 16-kHz mono audio
HOP_SAMPLES = 3840        # 240ms hop, 75% overlapping windows
WINDOW_BYTES = WINDOW_SAMPLES * 2
HOP_BYTES = HOP_SAMPLES * 2
MODEL_URL = "https://tfhub.dev/google/yamnet/1"
SIREN_NAMES = {"Siren", "Civil defense siren"}
HORN_NAMES = {"Vehicle horn, car horn, honking", "Air horn, truck horn", "Train horn"}


def say(msg):
    print(msg, flush=True)


class PacketParser:
    """Nonblocking, resynchronizing decoder for the Mini's AMS1 UART packets."""
    def __init__(self):
        self.buf = bytearray()
        self.invalid_headers = 0

    def push(self, data):
        self.buf.extend(data)
        packets = []
        while True:
            ix = self.buf.find(MAGIC)
            if ix < 0:
                # Preserve possible partial magic sequence at chunk boundary.
                if len(self.buf) > len(MAGIC) - 1:
                    del self.buf[:-(len(MAGIC) - 1)]
                break
            if ix > 0:
                del self.buf[:ix]
            if len(self.buf) < HEADER.size:
                break
            _, kind, length, seq = HEADER.unpack_from(self.buf)
            if kind not in (PKT_EVENT_START, PKT_FEATURE, PKT_PCM,
                            PKT_EVENT_END, PKT_HELLO) or length > MAX_PACKET_PAYLOAD:
                del self.buf[0]
                self.invalid_headers += 1
                continue
            end = HEADER.size + length
            if len(self.buf) < end:
                break
            payload = bytes(self.buf[HEADER.size:end])
            del self.buf[:end]
            packets.append((kind, seq, payload))
        return packets


def safe_open_uart(serial_module, port, baud):
    """Avoid intentionally toggling DTR/RTS, which can reset ESP32 boards."""
    ser = serial_module.Serial(port=None, baudrate=baud,
                               timeout=0.05, write_timeout=1.0)
    ser.dtr = False
    ser.rts = False
    ser.port = port
    ser.open()
    try:
        ser.setDTR(False)
        ser.setRTS(False)
    except (OSError, serial_module.SerialException):
        pass
    return ser


class KorvoBridge:
    """Dedicated heartbeat thread: not blocked by YAMNet inference or Mini I/O.

    If new positive classifications stop, send OFF after hold_sec.
    Hardware independently releases HAZARD if ON is not refreshed within 3s.
    """
    def __init__(self, ser, confirm, hold_sec, heartbeat_sec):
        self.ser = ser
        self.confirm = confirm
        self.hold_sec = hold_sec
        self.heartbeat_sec = heartbeat_sec
        self.consecutive = 0
        self.last_positive = 0.0
        self.active = False
        self.force_off = True
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.last_tx = 0.0
        self.tx_thread = threading.Thread(target=self._run_tx, name="hazard-heartbeat", daemon=True)
        self.rx_thread = (threading.Thread(target=self._run_rx, name="korvo-log", daemon=True)
                          if self.ser is not None else None)

    def start(self):
        self.tx_thread.start()
        if self.rx_thread:
            self.rx_thread.start()

    def observe(self, positive, siren, horn):
        now = time.monotonic()
        with self.lock:
            if positive:
                self.consecutive += 1
                self.last_positive = now
                if not self.active and self.consecutive >= self.confirm:
                    self.active = True
                    self.last_tx = 0.0
                    say(f"[HAZARD] CONFIRMED, SIREN={siren:.3f}, HORN={horn:.3f} "
                        f"({self.consecutive} consecutive overlapping windows)")
            else:
                self.consecutive = 0

    def reset(self, reason):
        with self.lock:
            self.active = False
            self.consecutive = 0
            self.last_positive = 0.0
            self.force_off = True
        say(f"[HAZARD] Clear: {reason}")

    def _write(self, command):
        if self.ser is not None:
            self.ser.write((command + "\n").encode("ascii"))
            self.ser.flush()
            say(f"[PI->S3] {command}")
        else:
            say(f"[OBSERVE ONLY] Would send {command}")

    def _run_tx(self):
        while not self.stop_event.is_set():
            now = time.monotonic()
            cmd = None
            with self.lock:
                if self.active and now - self.last_positive > self.hold_sec:
                    self.active = False
                    self.consecutive = 0
                    self.force_off = True
                    say(f"[HAZARD] Timeout: no recent positive window for {self.hold_sec:.1f}s")
                if self.force_off:
                    cmd = "HAZARD_OFF"
                    self.force_off = False
                    self.last_tx = now
                elif self.active and now - self.last_tx >= self.heartbeat_sec:
                    cmd = "HAZARD_ON"
                    self.last_tx = now
            if cmd:
                try:
                    self._write(cmd)
                except Exception as exc:
                    # S3 lease expires after 3s without successful renewal.
                    say(f"[ERROR] S3 serial write failed: {exc}")
                    self.stop_event.set()
                    break
            self.stop_event.wait(0.05)

    def _run_rx(self):
        while not self.stop_event.is_set():
            try:
                line = self.ser.readline().decode("utf-8", errors="replace").strip()
            except Exception as exc:
                if not self.stop_event.is_set():
                    say(f"[WARN] S3 serial read: {exc}")
                break
            if line and ("V8_UART" in line or "PLAYBACK phase" in line
                         or "PLAY_DONE" in line or "PHASE 3/6" in line
                         or "PHASE 5/6" in line or "STREAM_START" in line):
                say(f"[S3] {line}")

    def close(self):
        self.stop_event.set()
        if self.tx_thread.is_alive():
            self.tx_thread.join(timeout=1.5)
        # One explicit OFF before closing; S3 lease is fallback if UART fails.
        try:
            self._write("HAZARD_OFF")
        except Exception as exc:
            say(f"[WARN] Could not send final OFF: {exc}")
        if self.rx_thread and self.rx_thread.is_alive():
            self.rx_thread.join(timeout=0.5)


def model_classes(model, tf):
    path = model.class_map_path().numpy()
    if isinstance(path, bytes):
        path = path.decode("utf-8")
    with tf.io.gfile.GFile(path) as handle:
        return [r["display_name"] for r in csv.DictReader(handle)]


def main():
    p = argparse.ArgumentParser(description="V8.2 Audio-Master Mini -> YAMNet -> S3")
    p.add_argument("--port", default="/dev/ttyUSB1", help="Mini USB-UART input (921600bps)")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--s3-port", default="/dev/ttyUSB0", help="Korvo-2 USB-UART (115200bps)")
    p.add_argument("--s3-baud", type=int, default=115200)
    p.add_argument("--observe-only", action="store_true", help="Run classifier without sending to S3")
    p.add_argument("--horn-threshold", type=float, default=0.70)
    p.add_argument("--siren-threshold", type=float, default=0.65)
    p.add_argument("--confirm-windows", type=int, default=2)
    p.add_argument("--hazard-hold", type=float, default=1.5)
    p.add_argument("--heartbeat", type=float, default=0.30)
    p.add_argument("--auto-stop", type=float, default=5.0, help="Send STOP to Mini after N seconds; 0=disable")
    p.add_argument("--out", default=str(Path.home() / "captures_v8_2"))
    a = p.parse_args()
    if a.port == a.s3_port and not a.observe_only:
        p.error("Mini and S3 must not share a serial port")
    if a.confirm_windows < 1 or not 0.0 <= a.horn_threshold <= 1.0 or not 0.0 <= a.siren_threshold <= 1.0:
        p.error("Invalid thresholds or confirm-windows")
    if a.hazard_hold <= 0 or not 0.1 <= a.heartbeat <= 1.0 or a.auto_stop < 0:
        p.error("hold > 0, 0.1 <= heartbeat <= 1s, auto-stop >= 0 required")

    # Heavy imports and model load happen BEFORE opening Mini, so triggers
    # cannot arrive during model initialization and be silently discarded.
    import numpy as np
    import serial
    import tensorflow as tf
    import tensorflow_hub as hub

    say("[1/4] Loading YAMNet (wait for model to load)...")
    model = hub.load(MODEL_URL)
    names = model_classes(model, tf)
    siren_indices = [i for i, name in enumerate(names) if name in SIREN_NAMES]
    horn_indices = [i for i, name in enumerate(names) if name in HORN_NAMES]
    if not siren_indices or not horn_indices:
        raise RuntimeError("Could not locate expected horn/siren YAMNet classes")
    say(f"[2/4] SIREN: {[names[i] for i in siren_indices]}")
    say(f"      HORN : {[names[i] for i in horn_indices]}")
    say(f"      DEMO thresholds: horn>={a.horn_threshold}, siren>={a.siren_threshold}, "
        f"{a.confirm_windows} adjacent 75%-overlap windows")

    outdir = Path(a.out).expanduser()
    outdir.mkdir(parents=True, exist_ok=True)
    mini = None
    s3 = None
    bridge = None
    wav = None
    wav_path = None
    pcm = bytearray()
    parser = PacketParser()
    started_at = None
    stop_sent = False
    infer_no = 0
    last_pcm = None

    try:
        if not a.observe_only:
            s3 = safe_open_uart(serial, a.s3_port, a.s3_baud)
            say(f"[3/4] S3 UART OPEN: {a.s3_port} @ {a.s3_baud}")
        else:
            say("[3/4] OBSERVE ONLY, no S3 commands")
        bridge = KorvoBridge(s3, a.confirm_windows, a.hazard_hold, a.heartbeat)
        bridge.start()
        mini = safe_open_uart(serial, a.port, a.baud)
        say(f"[4/4] Mini UART OPEN: {a.port} @ {a.baud}")
        say("[READY] Waiting for Mini trigger; S3 currently plays only ~30s after RESET")
        say("[READY] To demo: RESET S3 now, then play a horn/siren sample near Mini")
        while True:
            chunk = mini.read(4096)
            now = time.monotonic()
            if chunk:
                packets = parser.push(chunk)
            else:
                packets = []
            for kind, seq, payload in packets:
                if kind == PKT_HELLO:
                    say(f"[HELLO] {payload.decode('utf-8', 'replace')}")

                elif kind == PKT_EVENT_START:
                    if wav is not None:
                        wav.close()
                    bridge.reset("new EVENT_START")
                    name = f"event_{time.strftime('%Y%m%d_%H%M%S')}_{seq}.wav"
                    wav_path = outdir / name
                    wav = wave.open(str(wav_path), "wb")
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(SAMPLE_RATE)
                    pcm.clear()
                    started_at = time.monotonic()
                    last_pcm = None
                    stop_sent = False
                    infer_no = 0
                    say(f"[EVENT_START] seq={seq}, save={wav_path}")

                elif kind == PKT_FEATURE:
                    if started_at is not None:
                        say(f"[FEATURE] {payload.decode('utf-8', 'replace')}")

                elif kind == PKT_PCM:
                    if wav is None or len(payload) % 2:
                        continue
                    wav.writeframesraw(payload)
                    pcm.extend(payload)
                    last_pcm = time.monotonic()
                    while len(pcm) >= WINDOW_BYTES:
                        samples = np.frombuffer(bytes(pcm[:WINDOW_BYTES]), dtype="<i2")
                        waveform = tf.convert_to_tensor(samples.astype(np.float32) / 32768.0, tf.float32)
                        t0 = time.monotonic()
                        scores, _, _ = model(waveform)
                        vec = scores.numpy().mean(axis=0)
                        siren = float(np.max(vec[siren_indices]))
                        horn = float(np.max(vec[horn_indices]))
                        top = int(np.argmax(vec))
                        infer_no += 1
                        classified_positive = siren >= a.siren_threshold or horn >= a.horn_threshold
                        bridge.observe(classified_positive, siren, horn)
                        say(f"[YAMNET {infer_no:02d}] SIREN={siren:.3f} HORN={horn:.3f} "
                            f"TOP={names[top]}({vec[top]:.3f}) "
                            f"positive={int(classified_positive)} "
                            f"infer_ms={(time.monotonic() - t0)*1000:.0f}")
                        del pcm[:HOP_BYTES]

                elif kind == PKT_EVENT_END:
                    reason = payload.decode('utf-8', 'replace')
                    if wav is not None:
                        wav.close()
                        wav = None
                    started_at = None
                    stop_sent = False
                    last_pcm = None
                    pcm.clear()
                    bridge.reset(f"EVENT_END ({reason})")
                    say(f"[EVENT_END] reason={reason}; saved={wav_path}")

            # Mini's firmware also ends events after 6s; send STOP sooner
            # for repeatable demo, preserving its existing protocol.
            if (a.auto_stop > 0 and started_at is not None and not stop_sent
                    and time.monotonic() - started_at >= a.auto_stop):
                mini.write(b"STOP\n")
                mini.flush()
                stop_sent = True
                say("[PI->Mini] STOP")

            # If an event silently stalls, clear stale hazard and wait
            # for the ESP32's built-in event timeout or next event.
            if (started_at is not None and last_pcm is not None
                    and time.monotonic() - last_pcm > 3.0):
                bridge.reset("Mini PCM stalled >3s")
                last_pcm = None

            if parser.invalid_headers:
                say(f"[WARN] UART packet resync: invalid_headers={parser.invalid_headers}")
                parser.invalid_headers = 0

    except KeyboardInterrupt:
        say("\n[QUIT] Ctrl+C received")
    finally:
        if bridge is not None:
            bridge.close()
        if wav is not None:
            wav.close()
        if mini is not None:
            mini.close()
        if s3 is not None:
            s3.close()
        say("[DONE] Serial ports closed; HAZARD_OFF requested")


if __name__ == "__main__":
    main()
