//전체 작업
/*PCM 읽기
↓
RMS
↓
dBFS
↓
ΔRMS
↓
Peak
↓
Trigger
↓
UART
즉, ESP 전체 pipeline*/
void app_main() //ESP 시작점, 실제로는 FreeRTOS task로 돌리는게 맞지만 현재는 설명용 단순화 상태
{
    uart_init();

    audio_init();
#include <stdio.h>

#include "config.h"
#include "rms_analysis.h"
#include "trigger_logic.h"
#include "uart_comm.h"

float prev_rms_db = -90.0f;

void process_audio_frame(
    int16_t *audio_buffer,
    int samples
)
{
    float rms =
        calculate_rms(
            audio_buffer,
            samples
        );

    float rms_db =
        calculate_dbfs(rms);

    float drms_db =
        calculate_drms(
            rms_db,
            prev_rms_db
        );

    prev_rms_db =
        rms_db;

    int16_t peak =
        calculate_peak(
            audio_buffer,
            samples
        );

    bool trigger =
        update_trigger_state(
            rms_db,
            drms_db,
            peak
        );

    send_uart_message(
        trigger,
        rms_db,
        drms_db,
        peak
    );
}

void app_main()
{
    while (1)
    {
        int16_t dummy_audio[512];

        process_audio_frame(
            dummy_audio,
            512
        );
    }
}
    while (1)
    {
        process_audio_frame();
    }
}