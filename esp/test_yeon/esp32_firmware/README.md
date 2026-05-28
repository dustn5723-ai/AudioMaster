# ESP32 Acoustic Trigger Firmware

This firmware performs lightweight acoustic triggering for emergency sound analysis.

## Features

- RMS(dBFS) analysis
- ΔRMS analysis
- Peak amplitude detection
- Loudness trend detection
- Hysteresis trigger control
- UART communication with Raspberry Pi

## Trigger Strategy

The ESP32 does NOT perform semantic analysis.

It only determines whether the current acoustic event is important enough to activate Raspberry Pi semantic inference.

## Trigger Parameters

- RMS(dBFS)
- ΔRMS
- Peak amplitude
- Increasing loudness trend

## UART Message Format

TRIGGER=1,RMS_DB=-24.5,DRMS_DB=4.1,PEAK=9321


마이크 입력
-> RCM Audio
-> RMS 계산
-> dBRS 변환
-> ΔRMS 계산
-> Peak 계산
-> Trigger 판단
-> UART 전송