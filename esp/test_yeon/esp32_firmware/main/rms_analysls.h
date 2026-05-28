#ifndef RMS_ANALYSIS_H
#define RMS_ANALYSIS_H

#include <stdint.h>

float calculate_rms(
    int16_t *buffer,
    int samples
);

float calculate_dbfs(
    float rms
);

float calculate_drms(
    float current_db,
    float previous_db
);

int16_t calculate_peak(
    int16_t *buffer,
    int samples
);

#endif