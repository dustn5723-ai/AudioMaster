#ESP <-> Pi 간 acoustic context 연결
import serial

ser = serial.Serial(  #UART ㅇ녀결
    "/dev/ttyUSB0",
    115200,
    timeout=0.1
)

trigger_active = False
#Pi는 항상 emantic inference 안돌림
#즉, ESP가 의미 있는 acoustic event 발견 시만 semantic analysis 수행한다.

latest_rms_db = -80.0 #현재 loudness -> YAMNet semantic만 보면 실제 거리/접근성을 확인할 수 없음 -> 그래서 acoustic 정보도 같이 사용
latest_drms_db = 0.0
latest_peak = 0

def read_uart(): #ESP 메시지 parsing pi가 현재 acoustic 상태를 알 수 있게

    global trigger_active
    global latest_rms_db 
    global latest_drms_db
    global latest_peak

    while ser.in_waiting > 0:

        line = ser.readline().decode(
            'utf-8',
            errors='ignore'
        ).strip()

        if "TRIGGER=" in line:

            parts = line.split(",")

            for p in parts:

                if "TRIGGER=" in p:

                    trigger_active = bool(
                        int(
                            p.split("=")[1]
                        )
                    )

                elif "RMS_DB=" in p:

                    latest_rms_db = float(
                        p.split("=")[1]
                    )

                elif "DRMS_DB=" in p:

                    latest_drms_db = float(
                        p.split("=")[1]
                    )

                elif "PEAK=" in p:

                    latest_peak = int(
                        p.split("=")[1]
                    )