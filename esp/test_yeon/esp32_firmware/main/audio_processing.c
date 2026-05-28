// 실제 마이크 PCM 데이터 읽기
#include "audio_processing.h"
#include "config.h"

#include "driver/i2s.h"

#define I2S_PORT I2S_NUM_1

void audio_init()
{
    i2s_config_t i2s_config = {     //ESP32의 I2S peripheral 설정 -> I2S digital microphone codec때문
        .mode =
            I2S_MODE_MASTER |
            I2S_MODE_RX,

        .sample_rate = SAMPLE_RATE,
        // 16kHz sampling을 사용

        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_16BIT,
            //16비트인 이유는 오디오 amplitude 정밀도 확보하기 위해

        .channel_format =
            I2S_CHANNEL_FMT_ONLY_LEFT,
            //모노 사용 -> YAMNet이 mono input 기준으로 학습되었기에
            //실시간 처리량 감소하기 위해

        .communication_format =
            I2S_COMM_FORMAT_STAND_I2S,

        .intr_alloc_flags = 0,

        .dma_buf_count = 8,

        .dma_buf_len = 256,

        .use_apll = false
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = 26,
        .ws_io_num = 25,
        .data_out_num = -1,
        .data_in_num = 33
    };

    i2s_driver_install(
        I2S_PORT,
        &i2s_config,
        0,
        NULL
    );

    i2s_set_pin(
        I2S_PORT,
        &pin_config
    );
}

void read_microphone_buffer(
    int16_t *buffer,
    int *samples
)
{
    size_t bytes_read = 0;

    i2s_read(
        I2S_PORT,
        buffer,
        AUDIO_BUFFER_SIZE,
        &bytes_read,
        portMAX_DELAY
    ); //실제 PCM 샘플 데이터 읽기 즉, 마이크->ESP RAM이다.

    *samples =
        bytes_read / sizeof(int16_t); //실제 읽은 byte 수 왜 샘플로 변환 bytes / sizeof(int16_t)을 해야 실제 sample 개수를 알 수 있기에
}