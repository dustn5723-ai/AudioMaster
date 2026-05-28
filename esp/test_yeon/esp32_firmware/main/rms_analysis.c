//Acoustic Feature Extraction 핵심
#include "rms_analysis.h"

#include <math.h>
#include <stdlib.h>

float calculate_rms( //RMS 공식 -> 신호의 평균 에너지
    int16_t *buffer,
    int samples
)
{
    int64_t sum = 0;

    for (int i = 0; i < samples; i++)
    {
        sum +=
            (int32_t)buffer[i] *
            (int32_t)buffer[i];
    }

    return sqrtf(
        (float)sum / samples
    );
}

float calculate_dbfs(float rms)
//PCM amplitude는 환경 따라 숫자 스케일이 달라짐
//그래서 정규화 loudness 표현가능
//왜 32768이냐면 16bit PCM 최대값이기에
//그래서 최대 dBFS는 0이다.
{
    return 20.0f *
        log10f(
            (rms + 1e-6f) / 32768.0f
        );
}

float calculate_drms(       //dram 계산
    float current_db,
    float previous_db
)
{
    return current_db - previous_db;
}

int16_t calculate_peak(     //peak는 순간 최대 amplitude이기에 짧고 강한 impulse특성이 있기에 그에 관한 계산이 필요
    int16_t *buffer,
    int samples
)
{
    int16_t peak = 0;

    for (int i = 0; i < samples; i++)
    {
        if (abs(buffer[i]) > peak)
        {
            peak = abs(buffer[i]);
        }
    }

    return peak;
}