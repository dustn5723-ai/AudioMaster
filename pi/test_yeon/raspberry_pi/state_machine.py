#Pi 전체 핵심 -> 단순 classifier가 아니라 Temporal State Machine이기 때문에
current_state = "NORMAL"

def update_state(
    S_siren,
    S_horn,
    C_siren,
    C_horn,
    R_db,
    D_db,
    rms_trend
):

    global current_state #현재 상황을 저장

    # =================================================
    # Near Siren
    # =================================================
    """
    조건:
    S_siren >= 0.60 -> 실제 사이렌 가능성이 높다.
    R_db >= -20 -> 가깝다.
    C_siren >= 0.60 -=> 지속되고있다.
    -> 즉 가까운 실제 사이렌을 의미하게 된다.
    """
    
    if (
        S_siren >= 0.60
        and
        R_db >= -20
        and
        C_siren >= 0.60
    ):
        current_state = "NEAR_SIREN"

        return 4, "NEAR_SIREN"

    # =================================================
    # Approaching
    # =================================================
    """
    조건:
    D_db >= 3
    or rms_trend >= 0.5

    -> 즉, 점점 접근 중
    """

    if (
        S_siren >= 0.35
        and
        C_siren >= 0.30
        and
        (
            D_db >= 3
            or rms_trend >= 0.5
        )
    ):
        current_state = "APPROACHING"

        return 3, "APPROACHING"

    # =================================================
    # Repeated Horn
    # =================================================
    """
    경적 반복 상황

    -> 즉, 운전자 경고 가능성
    """

    if (
        S_horn >= 0.25
        and
        C_horn >= 0.30
    ):
        current_state = "REPEATED_HORN"

        return 3, "REPEATED_HORN"

    # =================================================
    # Short Horn
    # =================================================
    """
    짧은 순간 경적

    -> 즉, 낮은 위험도
    """

    if (
        S_horn >= 0.25
    ):
        current_state = "SHORT_HORN"

        return 2, "SHORT_HORN"

    current_state = "NORMAL"
    """
    조건 불충족
    -> 즉, 위험 없음
    """

    return 0, "NORMAL"