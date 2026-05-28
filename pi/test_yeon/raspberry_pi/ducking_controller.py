#실제 volume control 명령
from uart_handler import ser

def send_ducking(level):
#pi가 최종 위험 level 결정 후 ESP에 보냄

    if level >= 4:

        ser.write(
            b"DUCKING:4\n"
        )

    elif level == 3:

        ser.write(
            b"DUCKING:3\n"
        )

    elif level == 2:

        ser.write(
            b"DUCKING:2\n"
        )

    elif level == 1:

        ser.write(
            b"DUCKING:1\n"
        )

    else:

        ser.write(
            b"NORMAL\n"
        )