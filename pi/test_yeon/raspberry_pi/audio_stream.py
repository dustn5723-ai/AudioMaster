#sliding window 핵심
#YAMNet은 고정 길이 입력이 필요하기에 최근 0.96초 유지하는 buffer가 필요하다.
import sounddevice as sd
import numpy as np

from collections import deque

SAMPLE_RATE = 16000

WINDOW_SEC = 0.96
HOP_SEC = 0.24

WINDOW_SIZE = int( #0.24초 단위 즉, 분석 overlap생김
    SAMPLE_RATE * WINDOW_SEC
)

HOP_SIZE = int(
    SAMPLE_RATE * HOP_SEC
)

audio_buffer = deque( #최근 audio만 유지 즉, FIFO circular buffer 구조
    maxlen=WINDOW_SIZE
)

def audio_callback( #sounddevice가 실시간 audio chunk을 보내줌 그걸 buffer에 저장한다.
    indata,
    frames,
    time_info,
    status
):

    mono = indata[:,0].astype(
        np.float32
    )

    audio_buffer.extend(mono)

stream = sd.InputStream(
    channels=1,
    samplerate=SAMPLE_RATE,
    blocksize=HOP_SIZE,
    callback=audio_callback
)

def start_audio_stream():

    stream.start()

def get_audio_window(): #0.96초 audio 반환 즉, YAMNet 입력 생성

    if len(audio_buffer) < WINDOW_SIZE:
        return None

    return np.array(
        audio_buffer,
        dtype=np.float32
    )