#!/usr/bin/env python3
"""Send manual S3 V8.3 UART test controls. Stop Pi YAMNet bridge first."""
import argparse
import time

p = argparse.ArgumentParser(description="S3 V8.3 manual mode control; requires exclusive S3 serial access")
p.add_argument("--port", default="/dev/ttyUSB0")
p.add_argument("--baud", type=int, default=115200)
p.add_argument("command", choices=["LOCAL_VAD_ON", "LOCAL_VAD_OFF", "AUDIO_ON", "AUDIO_OFF", "STATUS", "HAZARD_ON", "HAZARD_OFF"])
p.add_argument("--listen", type=float, default=1.5)
a = p.parse_args()

import serial
ser = serial.Serial(port=None, baudrate=a.baud, timeout=0.15, write_timeout=1)
ser.dtr = False
ser.rts = False
ser.port = a.port
ser.open()
try:
    ser.setDTR(False)
    ser.setRTS(False)
except (serial.SerialException, OSError):
    pass
try:
    ser.write((a.command + "\n").encode("ascii"))
    ser.flush()
    print(f"[AM][PI->S3][TX] {a.command}", flush=True)
    end = time.monotonic() + a.listen
    while time.monotonic() < end:
        msg = ser.readline().decode("utf-8", "replace").strip()
        if "[AM][S3][" in msg:
            print(msg[msg.index("[AM][S3]["):], flush=True)
finally:
    ser.close()
