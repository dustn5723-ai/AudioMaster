// 시스템의 threshold와 설정을 한 곳에서 관리
#ifndef CONFIG_H
#define CONFIG_H

// =====================================================
// Audio
// =====================================================

#define SAMPLE_RATE 16000
// 16kHz sampling
// YAMNet이 16kHz mono input을 기준으로 학습되었기에
// ESP <-> Pi sampling consistency를 위해 16kHz로 설정

#define AUDIO_BUFFER_SIZE 1024
// 한 번에 읽는 PCM 샘플 크기
// 실시간 시스템은 짧은 frame 단위로 처리해야 하기 대문에
// 대략 64ms정도

// =====================================================
// Trigger Threshold
// =====================================================

// Trigger ON
#define RMS_DB_ENTER_THRESHOLD   -35.0f
/* 
핵심이고 이 이상이면 충분히 큰소리라고 판단한다.
조용한 차량 -55~-45dB
일반 대화   -40~-30dB
가까운 경적 -25~-10dB
사이렌 접근 -30~--15dB

즉, -35dBFS 이상이면 실제 차량 환경에서 의미 있는 외부 이벤트일 가능성이 높다.
*/
#define DRMS_ENTER_THRESHOLD       4.0f
/*
갑작스럽게 4dB 이상 증가를 의미
사이렌이나 경적은 갑작기 접근 즉 단순 RMS보다 변화량이 더 중요하기에
*/

// Trigger OFF (Hysteresis)
#define RMS_DB_EXIT_THRESHOLD    -40.0f
/*
이건 hysteresis용
ON/OFF threshold 동일하면 ON OFF ON OFF 빠르게 튀게 된다.
그래서 ON/OFF threshold에 간격을 둬서 -35/-40정도의 threshold로 다르게 설정
*/
#define DRMS_EXIT_THRESHOLD        1.5f
/*
변화가 거의 없음이면 trigger 종료 가능
*/

#define PEAK_THRESHOLD          12000
/*
peak란 순간 최대 amplitude으로 경적은 짧고 매우 강한 impulsive sound일 수 있기에
즉, RMS는 낮아도 Peak는 매우 클 수 있음 그래서 peak도 본다.
*/

// =====================================================
// History
// =====================================================

#define HISTORY_SIZE 10
// 최근 RMS history 저장 -> 접근 추세를 판단하기 때문에

#endif