#Temporal filtering을 위한 confidence 계산 -> 순간 오탐 제거하기 위해 사용
from collections import deque

CONF_HISTORY = 10

siren_history = deque(
    maxlen=CONF_HISTORY
)

horn_history = deque(
    maxlen=CONF_HISTORY
)

def update_confidence_history(
    siren_active,
    horn_active
):

    siren_history.append(
        siren_active
    )

    horn_history.append(
        horn_active
    )

def calculate_confidence(history): # 최근 10개 중 7개가 siren이면 0.7이라는 값을 가짐 -> 즉, 지속적으로 들리는가를 판단 -> 실제 위험 상황은 시간적으로 지속되기에

    if len(history) == 0:
        return 0.0

    return (
        sum(history) / len(history)
    )

def get_siren_confidence():

    return calculate_confidence(
        siren_history
    )

def get_horn_confidence():

    return calculate_confidence(
        horn_history
    )