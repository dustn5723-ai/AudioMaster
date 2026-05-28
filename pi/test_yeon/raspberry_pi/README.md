# Raspberry Pi Semantic Audio System

This system performs semantic emergency sound analysis using YAMNet.

## Features

- Sliding Window Audio Analysis
- YAMNet Semantic Inference
- Temporal Confidence Estimation
- Semantic Transition Detection
- RMS Trend Analysis
- Hysteresis-based State Machine
- Real-time Ducking Control

## Audio Pipeline

ESP32 performs lightweight acoustic triggering.

Raspberry Pi performs heavy semantic inference only when trigger conditions are active.

## Sliding Window

- Window size: 0.96 sec
- Hop size: 0.24 sec

## Semantic Parameters

### S
YAMNet semantic score.

### C
Temporal confidence based on recent windows.

### R
Current RMS(dBFS).

### D
ΔRMS(dBFS).

## State Machine

NORMAL
→ FAR_SIREN
→ APPROACHING
→ NEAR_SIREN

## Semantic Transition

The system can detect:

- Siren
- Horn
- Siren → Horn transition
- Complex emergency situations

pi의 역할은 실제 위험 상황 semantic analysis이고
ESP는 큰 소리인가만 본다.
하지만 pi는 이게 실제 사이렌인가? 경적인가? 접근 중인가? 지속되는가? 복합 상황인가? 를 판단

pi의 전체 흐름
: ESP Trigger 수신
-> Sliding Window Audio
-> YAMNet
-> Semantic Score(S)
-> Confidence(C)
-> Trend Analysis
-> Semantic Transition
-> State Machine
-> Ducking Control