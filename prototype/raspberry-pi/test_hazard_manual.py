#!/usr/bin/env python3
"""Pi -> ESP32-S3 Korvo-2 UART0 manual hazard override test.

Requires: pip install pyserial
Pi USB -> Korvo USB-UART (115200, 8N1). Korvo should run V8.0 main.c.
Pi may reset Korvo when the port opens. Wait until phase 3/5 playback is audible,
then press Enter to trigger simulated hazard.
"""
import argparse
import sys
import threading
import time

import serial


def open_without_intentional_reset(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial(port=None, baudrate=baud, timeout=0.2, write_timeout=1)
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
    parser = argparse.ArgumentParser(description="Manual HAZARD_ON/OFF test for Audio-Master V8.0")
    parser.add_argument("--port", required=True, help="Korvo-2 serial port, e.g. /dev/ttyUSB1")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=2.0)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")

    ser = open_without_intentional_reset(args.port, args.baud)
    stop_read = threading.Event()

    def reader():
        while not stop_read.is_set():
            try:
                line = ser.readline().decode("utf-8", errors="replace").strip()
            except (OSError, serial.SerialException):
                break
            if line and any(word in line for word in ("V8_UART", "PHASE", "PLAYBACK", "STREAM_START", "PLAY_DONE")):
                print("[S3]", line, flush=True)

    worker = threading.Thread(target=reader, daemon=True)
    worker.start()

    def send(data: bytes):
        ser.write(data)
        ser.flush()

    try:
        print(f"[PI] Connected to Korvo on {args.port} @ {args.baud}")
        print("[PI] V8.0 still plays the 6-phase test for ~30 seconds and then goes silent.")
        print("[PI] When phase 3 or phase 5 speaker noise is audible, press Enter.")
        print("[PI] If playback already ended, press S3 RESET and wait for noise.")
        input("[PI] Press Enter to send simulated HAZARD_ON... ")
        end = time.monotonic() + args.seconds
        print(f"[PI] HAZARD_ON for {args.seconds:.1f}s (heartbeat every 0.25s)")
        while time.monotonic() < end:
            send(b"HAZARD_ON\n")
            time.sleep(min(0.25, max(0, end - time.monotonic())))
        send(b"HAZARD_OFF\n")
        print("[PI] HAZARD_OFF sent; listening for two seconds...")
        time.sleep(2.0)
    except KeyboardInterrupt:
        print("\n[PI] Interrupted")
    finally:
        try:
            send(b"HAZARD_OFF\n")
        except (OSError, serial.SerialException):
            pass
        stop_read.set()
        worker.join(timeout=0.5)
        ser.close()
        print("[PI] Closed Korvo serial")


if __name__ == "__main__":
    main()
