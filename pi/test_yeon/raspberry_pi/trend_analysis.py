#접근성 분석
import numpy as np

from collections import deque

rms_history = deque(maxlen=10)
#최근 loudness 저장 -> 접근 차량은 점점 커지기에

def update_rms_history(rms_db):

    rms_history.append(rms_db)

def calculate_rms_trend(): #기울기 계산 -> 단순 loudness보다 변화 방향이 중요하기에

    if len(rms_history) < 4:
        return 0.0

    y = np.array(
        rms_history,
        dtype=np.float32
    )

    x = np.arange(len(y))

    slope, intercept = np.polyfit(
        x,
        y,
        1
    )

    return slope