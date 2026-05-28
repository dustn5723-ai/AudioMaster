#상황 변화 분석 -> siren과 horn이 동시에 들리는가, siren만 들리는가, horn만 들리는가, 둘 다 안 들리는가를 판단 -> 실제 위험 상황은 siren과 horn이 동시에 들리는 경우이기에
#실제 차량 환경은 단일 sound만 존재하지 않기에
#즉, 복합 semantic 상황판단이 필요하기에
from collections import deque

state_history = deque( 

)

def update_state_history(state):

    state_history.append(state)
    #최근 semantic상태 저장 -> 단일 window의 판단이 아니라 최근 상황의 변화까지 고려하기에

def detect_semantic_transition():
    #had_siren and had_horn이면 COMPLEX 상황판단 -> 즉, 복합 위험 상황

    recent = list(state_history)

    had_siren = any(
        s in [
            "FAR_SIREN",
            "APPROACHING",
            "NEAR_SIREN"
        ]
        for s in recent
    )

    had_horn = any(
        s in [
            "SHORT_HORN",
            "REPEATED_HORN"
        ]
        for s in recent
    )

    # ============================================
    # Siren + Horn
    # ============================================

    if had_siren and had_horn:

        return "COMPLEX"

    # ============================================
    # Siren only
    # ============================================

    if had_siren:

        return "SIREN"

    # ============================================
    # Horn only
    # ============================================

    if had_horn:

        return "HORN"

    return "NORMAL"