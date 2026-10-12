#!/usr/bin/env python3
import os
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import argparse
import csv
import struct
import time
import wave
from pathlib import Path

import numpy as np
import serial
import tensorflow as tf
import tensorflow_hub as hub

MODEL_URL = "https://tfhub.dev/google/yamnet/1"

MAGIC = b"AMS1"
HDR = struct.Struct("<4sBHI")

PKT_EVENT_START = 0x01
PKT_FEATURE     = 0x02
PKT_PCM         = 0x03
PKT_EVENT_END   = 0x04
PKT_HELLO       = 0x05

SR = 16000
WINDOW_SAMPLES = int(0.96 * SR)   # 15360
HOP_SAMPLES    = int(0.24 * SR)   # 3840 = 75% overlap

SIREN_NAMES = {
    "Siren",
    "Civil defense siren",
}
HORN_NAMES = {
    "Vehicle horn, car horn, honking",
    "Air horn, truck horn",
    "Train horn",
}

def read_exact(ser, n):
    data = bytearray()
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if chunk:
            data.extend(chunk)
    return bytes(data)

def find_header(ser):
    window = bytearray()
    while True:
        b = ser.read(1)
        if not b:
            continue
        window += b
        if len(window) > 4:
            del window[0]
        if bytes(window) == MAGIC:
            rest = read_exact(ser, HDR.size - 4)
            return HDR.unpack(MAGIC + rest)

def class_names_from_model(model):
    p = model.class_map_path().numpy()
    if isinstance(p, bytes):
        p = p.decode("utf-8")
    names = []
    with tf.io.gfile.GFile(p) as f:
        for row in csv.DictReader(f):
            names.append(row["display_name"])
    return names

def idxs(names, wanted):
    return [i for i, n in enumerate(names) if n in wanted]

def max_group(score_vec, indices):
    if not indices:
        return 0.0
    return float(np.max(score_vec[indices]))

def open_hazard_serial(port, baud):
    # Deassert modem-control lines where possible to avoid unintended reset.
    ser = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=1)
    ser.dtr = False
    ser.rts = False
    ser.port = port
    ser.open()
    try:
        ser.setDTR(False)
        ser.setRTS(False)
    except (OSError, serial.SerialException):
        pass
    return ser


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--out", default=str(Path.home() / "captures_yamnet"))
    ap.add_argument("--auto-stop", type=float, default=5.0,
                    help="send STOP this many seconds after EVENT_START; 0 disables")
    ap.add_argument("--s3-port", default=None,
                    help="Korvo-2 USB-UART serial port for HAZARD_ON/OFF (115200bps)")
    ap.add_argument("--s3-baud", type=int, default=115200)
    ap.add_argument("--horn-threshold", type=float, default=0.70,
                    help="DEMO threshold only; not a calibrated probability")
    ap.add_argument("--siren-threshold", type=float, default=0.65,
                    help="DEMO threshold only; not a calibrated probability")
    ap.add_argument("--confirm-windows", type=int, default=2,
                    help="successive overlapping YAMNet windows, not independent evidence")
    ap.add_argument("--hazard-hold", type=float, default=1.2,
                    help="seconds since most recent positive window before sending OFF")
    args = ap.parse_args()
    if args.s3_port and args.s3_port == args.port:
        ap.error("Mini and S3 serial ports MUST be different")
    if args.confirm_windows < 1 or args.hazard_hold <= 0:
        ap.error("confirm-windows must be >=1; hazard-hold must be positive")

    print("[1/3] Loading YAMNet...")
    model = hub.load(MODEL_URL)
    names = class_names_from_model(model)
    siren_idx = idxs(names, SIREN_NAMES)
    horn_idx = idxs(names, HORN_NAMES)

    print("[2/3] Target classes")
    print("  Siren:", [names[i] for i in siren_idx])
    print("  Horn :", [names[i] for i in horn_idx])

    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    print(f"[3/3] OPEN {args.port} @ {args.baud}")
    print("Waiting for Mini trigger...")
    s3 = open_hazard_serial(args.s3_port, args.s3_baud) if args.s3_port else None
    if s3:
        print(f"[S3] OPEN {args.s3_port} @ {args.s3_baud} (V8.0 only; not V7.3)")
        s3.write(b"HAZARD_OFF\n")
        s3.flush()
    else:
        print("[S3] --s3-port not provided; classify only, no ducking commands")

    hazard_on = False
    positive_windows = 0
    last_positive_at = None
    last_heartbeat_at = 0.0

    def command(cmd):
        if s3:
            s3.write((cmd + "\n").encode("ascii"))
            s3.flush()
            print(f"[PI->S3] {cmd}")

    def hazard_off():
        nonlocal hazard_on, positive_windows, last_positive_at
        if hazard_on:
            command("HAZARD_OFF")
        hazard_on = False
        positive_windows = 0
        last_positive_at = None

    wav = None
    wav_path = None
    sample_buffer = np.empty(0, dtype=np.int16)
    event_started_at = None
    stop_sent = False
    infer_no = 0

    try:
        while True:
            _, ptype, length, seq = find_header(ser)
            payload = read_exact(ser, length) if length else b""

            if ptype == PKT_HELLO:
                print("[HELLO]", payload.decode(errors="replace"))

            elif ptype == PKT_EVENT_START:
                if wav is not None:
                    wav.close()
                stamp = time.strftime("%Y%m%d_%H%M%S")
                wav_path = outdir / f"yamnet_event_{stamp}.wav"
                wav = wave.open(str(wav_path), "wb")
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(SR)
                sample_buffer = np.empty(0, dtype=np.int16)
                event_started_at = time.monotonic()
                stop_sent = False
                infer_no = 0
                hazard_off()
                print(f"\n[EVENT_START] seq={seq}")

            elif ptype == PKT_FEATURE:
                print("[FEATURE]", payload.decode(errors="replace"))

            elif ptype == PKT_PCM:
                if wav is not None:
                    wav.writeframesraw(payload)

                samples = np.frombuffer(payload, dtype="<i2")
                sample_buffer = np.concatenate((sample_buffer, samples))

                while len(sample_buffer) >= WINDOW_SAMPLES:
                    window_i16 = sample_buffer[:WINDOW_SAMPLES]
                    waveform = window_i16.astype(np.float32) / 32768.0

                    scores, _, _ = model(tf.convert_to_tensor(waveform, tf.float32))
                    score_vec = scores.numpy().mean(axis=0)

                    siren = max_group(score_vec, siren_idx)
                    horn = max_group(score_vec, horn_idx)
                    top_i = int(np.argmax(score_vec))
                    infer_no += 1

                    print(
                        f"[YAMNET {infer_no:02d}] "
                        f"SIREN={siren:.3f} HORN={horn:.3f} "
                        f"TOP={names[top_i]}({score_vec[top_i]:.3f})"
                    )

                    # Separate experimental decision stage. Scores are model
                    # outputs, not calibrated real-world hazard probabilities.
                    now = time.monotonic()
                    positive = (siren >= args.siren_threshold or horn >= args.horn_threshold)
                    if positive:
                        positive_windows += 1
                        last_positive_at = now
                    else:
                        positive_windows = 0

                    if not hazard_on and positive_windows >= args.confirm_windows:
                        hazard_on = True
                        command("HAZARD_ON")
                        last_heartbeat_at = now
                    elif hazard_on and last_positive_at is not None and now - last_positive_at > args.hazard_hold:
                        hazard_off()
                    elif hazard_on and now - last_heartbeat_at >= 0.4:
                        command("HAZARD_ON")   # renew S3's 3s lease
                        last_heartbeat_at = now

                    # 0.24 s hop -> 75% overlap.
                    sample_buffer = sample_buffer[HOP_SAMPLES:]

            elif ptype == PKT_EVENT_END:
                reason = payload.decode(errors="replace")
                if wav is not None:
                    wav.close()
                    wav = None
                print(f"[EVENT_END] reason={reason} saved={wav_path}\n")
                event_started_at = None
                stop_sent = False
                sample_buffer = np.empty(0, dtype=np.int16)
                hazard_off()

            if (args.auto_stop > 0 and event_started_at is not None and not stop_sent
                    and time.monotonic() - event_started_at >= args.auto_stop):
                ser.write(b"STOP\n")
                ser.flush()
                stop_sent = True
                print("[TX] STOP")

    except KeyboardInterrupt:
        print("\n[QUIT]")
    finally:
        try:
            command("HAZARD_OFF")
        except (OSError, serial.SerialException):
            pass
        if s3:
            s3.close()
        if wav is not None:
            wav.close()
        ser.close()

if __name__ == "__main__":
    main()
