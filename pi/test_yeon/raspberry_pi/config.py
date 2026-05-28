#시스템 전체 parameter 관리
SAMPLE_RATE = 16000
#16kHz인 이유는 YAMNet이 16kHz mono audio를 기준으로 학습되었기에
#sampling mismatch 방지하기 위해
WINDOW_SEC = 0.96 #0.96초 오디오를 한 번에 분석하기위해 -> YAMNet architecture 자체 기준이기에
HOP_SEC = 0.24 #0.24초 간격으로 오디오를 분석하기 위해 -> 실시간 상황 변화 감지 가능 -> 사이렌 접근과 같은 temporal 변화 추적 가능

CONF_HISTORY = 10 #이전 상태를 저장할 기록 수 -> 순간 오탐과 경향성을 확인할 수 있다.

TH_SIREN = 0.35 #semantic score가 0.35이상이면 사이렌 가능성이 있음
TH_HORN = 0.25 #semantic score가 0.25이상이면 허니 가능성이 있음