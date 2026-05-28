//ESP 핵심 판단 로직을 담당
#include "trigger_logic.h"
#include "config.h"

static bool trigger_active = false;
//현재 trigger 상태 저장

static float rms_history[HISTORY_SIZE];
//rms_history 최근 RMS값 저장 -> 접근 추세 판단하기 위해 -> 접근 트렌드를 확인하기 위해

static int history_index = 0;

bool check_increasing_trend()
//최근 3개 frame이 계속 증가하는가를 판단

{
    int idx0 =
        (history_index - 1 + HISTORY_SIZE)
        % HISTORY_SIZE;

    int idx1 =
        (history_index - 2 + HISTORY_SIZE)
        % HISTORY_SIZE;

    int idx2 =
        (history_index - 3 + HISTORY_SIZE)
        % HISTORY_SIZE;

    return (
        rms_history[idx2] <
        rms_history[idx1] &&
        rms_history[idx1] <
        rms_history[idx0]
    );
}

bool update_trigger_state(
    float rms_db,
    float drms_db,
    int16_t peak
)
{
    rms_history[history_index] =
        rms_db;

    history_index =
        (history_index + 1)
        % HISTORY_SIZE;

    bool increasing_trend =
        check_increasing_trend();

    // =================================================
    // Trigger OFF → ON
    // =================================================

    if (!trigger_active)
    {
        bool enter_condition = false;

        if (
            rms_db >
            RMS_DB_ENTER_THRESHOLD
        )
        {
            enter_condition = true;
        }

        if (
            drms_db >
            DRMS_ENTER_THRESHOLD
        )
        {
            enter_condition = true;
        }

        if (increasing_trend)
        {
            enter_condition = true;
        }

        if (
            peak >
            PEAK_THRESHOLD
        )
        {
            enter_condition = true;
        }

        if (enter_condition)
        {
            trigger_active = true;
        }
    }

    // =================================================
    // Trigger ON → OFF
    // =================================================

    else
    {
        bool exit_condition = false;

        if (
            rms_db <
            RMS_DB_EXIT_THRESHOLD
            &&
            drms_db <
            DRMS_EXIT_THRESHOLD
            &&
            !increasing_trend
        )
        {
            exit_condition = true;
        }

        if (exit_condition)
        {
            trigger_active = false;
        }
    }
/*
Trigger OFF -> ON 조건

1. RMS 큼
2. ΔRMS 큼
3. Trend 존재
4. Peak 큼
중 하나

OR인 이유는 실제 위험 상황은 다양하니

Trigger ON -> OFF 조건

RMS 낮음 AND ΔRMS 작음 AND trend 없음

AND인 이유는 하나라도 의미 있으면 아직 상황 유지 가능
*/
    return trigger_active;
}