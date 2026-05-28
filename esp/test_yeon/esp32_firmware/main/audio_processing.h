#ifndef AUDIO_PROCESSING_H
#define AUDIO_PROCESSING_H

#include <stdint.h>

void audio_init();

void read_microphone_buffer(
    int16_t *buffer,
    int *samples
);

#endif