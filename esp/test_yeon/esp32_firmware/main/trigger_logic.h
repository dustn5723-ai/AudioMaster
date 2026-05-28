#ifndef TRIGGER_LOGIC_H
#define TRIGGER_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

bool update_trigger_state(
    float rms_db,
    float drms_db,
    int16_t peak
);

#endif