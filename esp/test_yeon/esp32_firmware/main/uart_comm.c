//pi와의 통신
//UART는 ESP ↔ Pi serial communication
/*메시지 형식 "TRIGGER=1,RMS_DB=-24.5,DRMS_DB=4.1,PEAK=9321"
왜 이렇게 보내냐면 pi가 실시간 acoustic context 알 수 있게 하기 위해
즉, pi는 semantic만 보는 게 아니라 acoustic context도 함꼐 오기에*/
#include "uart_comm.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"

void send_uart_message(
    bool trigger,
    float rms_db,
    float drms_db,
    int16_t peak
)
{
    char msg[128];

    sprintf(
        msg,
        "TRIGGER=%d,RMS_DB=%.2f,DRMS_DB=%.2f,PEAK=%d\n",
        trigger ? 1 : 0,
        rms_db,
        drms_db,
        peak
    );

    uart_write_bytes(
        UART_NUM_0,
        msg,
        strlen(msg)
    );
}