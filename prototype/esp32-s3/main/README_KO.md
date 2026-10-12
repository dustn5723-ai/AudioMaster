# Audio-Master V8.4: 음악 재생 중 AEC + VAD 오탐 검증

## 이 버전의 목적

V8.3의 **S3 연속 재생·AEC·VAD·UART·PCM Ducking**을 유지하고, PRBS 잡음만 실제 음악으로 교체하는 진단 버전입니다. **ESP32 Mini 펌웨어와 Raspberry Pi YAMNet 코드는 변경하지 않습니다.** 현재는 사용자의 음악과 기침/음성 등을 올바르게 구분한다고 보장할 수 없습니다. 노래 자체에 보컬이 있으므로 AEC가 재생 에코를 충분히 지우지 못하면 내부 VAD가 음악을 사람 음성으로 오인할 수 있습니다.

- 원본 입력: 사용자가 업로드한 소연(SOYEON) '퇴사할게여' WAV
- 추출: 원본 **5~19초**, 총 **14초** (반주와 초반 보컬 포함). 16kHz, mono, 16-bit signed little-endian PCM
- 정적 신호 이득: 0.46, 시작·끝 60ms 페이드. 디지털 최대치 15358/32767, RMS 약 3655 PCM
- 음악 PCM 파일 크기: **448000 바이트** (ESP-IDF 플래시 앱 파티션에 포함)
- 스피커 출력: 음악 루프 → 현재 Ducking 게인 적용 → ES8311, **동일한 게인 적용 PCM을 AEC 참조**로 저장
- 기존 `HAZARD_ON/OFF`, `LOCAL_VAD_ON/OFF`, `AUDIO_ON/OFF`, `STATUS` 명령 유지 (UART0 115200)
- `AUDIO_OFF`에서도 재생 위치는 진행하지만 출력과 AEC 참조에는 0이 들어감

**주의:** 음악의 볼륨이 큰 것과 외부 사람이 말하는 것은 다릅니다. WebRTC VAD의 입력에 잔류 음악이 남으면 보컬을 음성으로 볼 수 있고, 기존 저역량 RMS 트리거로 -6dB가 잘못 발생할 수 있습니다. V8.4는 이를 *측정하는 버전*이며, 임계값 튜닝이나 오류 개선 전 버전입니다.

## 0. 이전 성공 버전 보존

V8.3은 사용자가 실제 빌드·플래시했고 연속 소리 출력에 성공한 버전입니다. 다음 설치 스크립트는 `main.c`와 `main/CMakeLists.txt`를 프로젝트 **밖의** `C:\esp\backup\before_v8_4_날짜`에 자동 백업합니다. `main_v8_4.c`를 이름 그대로 프로젝트에 추가만 하면 `main.c`가 빌드되어 이전 V8.0이 올라갈 수 있으니 주의하세요.

## 1. Windows 설치

1. V8.4 ZIP을 Windows **다운로드 폴더**에 저장 후 파일 탐색기로 압축 풀기.
2. **Windows ESP-IDF PowerShell**을 새로 열기. (Pi SSH 창이 아님)
3. 압축을 풀어 `install_v8_4.ps1`이 있는 폴더에서:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_v8_4.ps1
```

4. 마지막에 `INSTALLED`와 `MUSIC PCM` 경로, 백업 경로가 출력되는지 확인.
5. 같은 ESP-IDF PowerShell에서:

```powershell
cd C:\esp\projects\SmartSpeaker_Internal_DuplexTest
idf.py reconfigure
idf.py build
```

처음 컴파일은 오래 걸릴 수 있습니다. `idf.py build` 결과가 **성공인지 확인한 뒤에만** 다음 실행:

```powershell
idf.py -p COM4 flash monitor
```

`COM4`는 현재 장치관리자에서 S3 포트가 맞을 때만 사용. **S3의 USB-UART는 Raspberry Pi에서 분리해서 Windows에 연결.** USB-PWR는 기존 방식 유지.

**실제 ESP-IDF 5.4.4 하드웨어 빌드는 이 제작 환경에서 실행하지 않았으며 사용자 PC에서 검증이 필요합니다.** 빌드 결과 앱 파티션 0x100000을 초과하면 플래시하지 말고 결과를 보내 주세요. 기존 파티션 설정을 임의로 키우지 말 것.

수동 설치를 원한다면 `main_v8_4.c`를 프로젝트 `main/main.c`로 이름을 변경해 덮어쓰고, `music_16k_mono.pcm`를 같은 `main/` 폴더에 복사하고, `main/CMakeLists.txt`의 `idf_component_register(...)` **뒤**에 아래 한 줄을 추가하세요. 기존 컴포넌트 의존성은 유지해야 합니다.

```cmake
target_add_binary_data(${COMPONENT_LIB} "music_16k_mono.pcm" BINARY)
```

## 2. 단독 테스트 계획 (Mini와 Pi는 연결하지 않아도 됨)

**A. 음악 단독 재생 (거짓 Ducking 검사)**

- S3 모니터 실행 중 아무 말도 하지 않고 **60초** 이상 테스트.
- `[AM][S3][MUSIC]`, `[AM][S3][AUDIO]`, `[AM][S3][VAD]`, `[AM][S3][DUCK]`, `[AM][S3][TELEMETRY]` 확인.
- 음악만 재생 중 `source=LOCAL_VAD to_db=-6/-12`가 나타나면 **오탐 후보**. 음악에 보컬이 포함돼 있어 그런 현상이 발생할 수 있음.
- `raw_dbfs`와 `clean_dbfs` 차이는 참고자료이며, 그 자체만으로 AEC 음질 또는 ERLE 성능 입증 불가.

**B. 음악+외부 사람 목소리**

- 음악이 나오는 상태에서 **일정한 거리**에서 5초간 말하기 → 5초 침묵 → 반복.
- `confirmed=VOICE` 및 `source=LOCAL_VAD` 동작과 회복 시간 확인.
- 반드시 음악 단독 시험과 로그를 구분해서 기록.

**C. 분리 모드**

음악을 듣되 자동 감쇠를 잠시 꺼서 원인을 구분해야 한다면 S3 모니터를 `Ctrl + ]`로 닫은 뒤 (시리얼 포트를 동시에 둘이서 열 수 없음) **Pi에 S3를 연결하거나**, Windows ESP-IDF Python의 `pyserial`을 사용해서 포함된 `s3_control.py`로 명령 전송:

```powershell
python .\s3_control.py --port COM4 LOCAL_VAD_OFF
python .\s3_control.py --port COM4 STATUS
python .\s3_control.py --port COM4 LOCAL_VAD_ON
```

`LOCAL_VAD_OFF`는 진단 기능만 차단하며 **AEC/VAD 분석은 계속** 수행합니다. 다시 `LOCAL_VAD_ON`으로 복구해야 실제 음성 감지에 따른 Ducking을 시험할 수 있습니다. `AUDIO_OFF`는 스피커 음악만 끄므로 외부 소음과 VAD를 비교할 때 사용합니다.

## 3. 보드 간 전체 통합

S3 음악 단독 시험이 끝나면 V8.3 때처럼 S3 USB-UART를 Pi `/dev/ttyUSB0`로 연결하고 Mini는 기존 `/dev/ttyUSB1`에 연결합니다. Pi의 `yamnet_live_hazard_v8_2.py` 또는 `yamnet_live_hazard_v8_3_view.py`는 별도 수정하지 않습니다. 경적 판정이 연속 2개 창에서 충족되어야 Pi가 `HAZARD_ON`을 송신합니다. 이전에 한 창에서 `HORN=0.936`만 나온 것은 자동 제어 성공이 아니었습니다.

## 4. 기록으로 남겨야 할 것

1. **음악만 60초:** 불필요한 VAD 확정·Ducking 발생 횟수
2. **음악+말하기 5초 반복:** 검출 및 복귀 시각, 오탐·미탐
3. **외부 경적:** Pi 분류 → `HAZARD_ON` → 음악 −18dB 감쇠 및 복귀
4. **AEC 처리:** `aec_avg_us`, `aec_max_us`, `slow_frames` 및 마이크 `raw_dbfs`/`clean_dbfs`

`−12dB/−18dB`은 출력 PCM의 **목표 감쇠량**이며 실제 SPL 측정값이 아닙니다.

## 5. 알려진 한계

- AEC 참조가 실제로 디지털 PCM과 연결돼 있지만 참조 위치는 기존 V8.3의 `ref_lag_samples=370` 추정치를 사용하므로 음악의 보컬·드럼 등을 완전 제거한다고 보장할 수 없음.
- 음악 자체에 보컬이 있어 단순 VAD만으로 '외부 사람 목소리'와 '음악 가창'을 언제나 구별할 수 없음.
- 현재 `RMS > 85 PCM` 게이트가 음악 잔류 신호에 의해 반응할 수 있으므로 추가 개선 대상.
- 음악 구간을 14초 루프하므로 곡 전체를 재생하는 것은 아니며, 페이드 접합 부분이 반복됨.
- 프로젝트는 실내 시제품 시연 목적. 자동차 실제 운행 중 이용할 수 있는 검증된 안전 장치가 아님.
