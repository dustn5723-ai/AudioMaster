
#!/usr/bin/env python3
import argparse
import os
import struct
import time
import wave
from pathlib import Path

import serial

MAGIC = b"AMS1"
HDR = struct.Struct("<4sBHI")  # magic, type, payload length, sequence

PKT_EVENT_START = 0x01
PKT_FEATURE     = 0x02
PKT_PCM         = 0x03
PKT_EVENT_END   = 0x04
PKT_HELLO       = 0x05

SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2

def read_exact(ser, n):
    data = bytearray()
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            continue
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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--out", default="captures")
    ap.add_argument("--auto-stop", type=float, default=4.0,
                    help="seconds after EVENT_START to send STOP; 0 disables")
    args = ap.parse_args()

    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    print(f"[OPEN] {args.port} @ {args.baud}")

    wav = None
    wav_path = None
    event_started_at = None
    stop_sent = False

    try:
        while True:
            magic, ptype, length, seq = find_header(ser)
            payload = read_exact(ser, length) if length else b""

            if ptype == PKT_HELLO:
                print("[HELLO]", payload.decode(errors="replace"))

            elif ptype == PKT_EVENT_START:
                stamp = time.strftime("%Y%m%d_%H%M%S")
                wav_path = outdir / f"event_{stamp}.wav"
                wav = wave.open(str(wav_path), "wb")
                wav.setnchannels(CHANNELS)
                wav.setsampwidth(SAMPLE_WIDTH)
                wav.setframerate(SAMPLE_RATE)
                event_started_at = time.monotonic()
                stop_sent = False
                print(f"[EVENT_START] seq={seq} -> {wav_path}")

            elif ptype == PKT_FEATURE:
                print("[FEATURE]", payload.decode(errors="replace"))

            elif ptype == PKT_PCM:
                if wav is not None:
                    wav.writeframesraw(payload)

            elif ptype == PKT_EVENT_END:
                reason = payload.decode(errors="replace")
                if wav is not None:
                    wav.close()
                    wav = None
                    print(f"[EVENT_END] reason={reason} saved={wav_path}")
                event_started_at = None
                stop_sent = False

            if (args.auto_stop > 0 and event_started_at is not None and not stop_sent
                    and time.monotonic() - event_started_at >= args.auto_stop):
                ser.write(b"STOP\n")
                ser.flush()
                stop_sent = True
                print("[TX] STOP")

    except KeyboardInterrupt:
        print("\n[QUIT]")
    finally:
        if wav is not None:
            wav.close()
        ser.close()

if __name__ == "__main__":
    main()
