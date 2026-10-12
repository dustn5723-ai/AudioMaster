# Audio-Master Prototype

차량용 위험음 인식 및 오디오 Ducking 시스템

## 폴더 구성

- esp32-mini: Fast Trigger, 2초 Ring Buffer, UART PCM 송신
- raspberry-pi: YAMNet 음향 분류 및 위험음 제어
- esp32-s3: AEC, VAD, 음악 재생 및 PCM Ducking

## 통신 설정

- Mini → Pi: UART 921600bps
- Pi → S3: UART 115200bps
- 오디오: 16kHz, 16-bit, Mono
- YAMNet: 0.96초 Window, 0.24초 Hop

## 현재 개발 상태

완료:
- Mini → Pi PCM 전송
- Raspberry Pi YAMNet 실시간 분석
- S3 음악 재생 및 음성 Ducking
- Pi → S3 수동 HAZARD_ON/OFF 제어

추가 검증 필요:
- Mini → Pi → S3 전체 자동 위험음 Ducking
- 검출 정확도, 오탐률, 전체 응답시간

## 참고

S3 원본 프로젝트의 music_16k_mono.pcm은
저작권 문제로 제외했습니다.

S3 코드를 빌드하려면 main 폴더에
16kHz / 16-bit / Mono PCM 파일을 별도로 준비해야 합니다.

ESP32 Mini는 ESP-ADF 관련 의존성이 필요합니다.
