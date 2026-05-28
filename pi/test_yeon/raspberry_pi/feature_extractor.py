#semantic Feature Extraction -> 핵심
from collections import deque

SIREN_CLASSES = [
    316,
    317,
    318,
    319,
    390
]

HORN_CLASSES = [
    302
]

siren_history = deque(maxlen=10)
horn_history = deque(maxlen=10)

def get_score( #각 class의 최대 semantic activation사용
    scores_np,
    class_indices
):
    vals = []

    for idx in class_indices:

        vals.append(
            scores_np[:, idx].max() #max인 이유는 경적은 짧은 순간 매우 강한 activation이기에 ->mean을 쓰면 희석이 되기에
        )

    return max(vals)

def calculate_confidence(history):

    if len(history) == 0:
        return 0.0

    return sum(history) / len(history)

def extract_features(scores_np):

    S_siren = get_score( #현재 window의 semantic siren 가능성
        scores_np,
        SIREN_CLASSES
    )

    S_horn = get_score( #현재 window의 semantic horn 가능성
        scores_np,
        HORN_CLASSES
    )

    siren_active = ( #현재 window가 siren인가를 확인
        1 if S_siren >= 0.35
        else 0
    )

    horn_active = (
        1 if S_horn >= 0.25
        else 0
    )

    siren_history.append(
        siren_active
    )

    horn_history.append(
        horn_active
    )

    C_siren = calculate_confidence( #최근 10개 window 중 몇 번 siren이었는가 즉 Temporal persistence를 측정 -> 순간 오탐 제거하기 위해 사용
        siren_history
    )

    C_horn = calculate_confidence(
        horn_history
    )

    return (
        S_siren,
        S_horn,
        C_siren,
        C_horn
    )