#전체 orchestration
import time

import uart_handler

from audio_stream import (
    start_audio_stream,
    get_audio_window
)

from yamnet_model import run_yamnet

from feature_extractor import extract_features

from trend_analysis import (
    update_rms_history,
    calculate_rms_trend
)

from state_machine import update_state

from ducking_controller import send_ducking

from semantic_transition import (
    update_state_history,
    detect_semantic_transition
)

print("Pi Semantic System Start")

start_audio_stream()

while True:

    uart_handler.read_uart()

    if not uart_handler.trigger_active:

        time.sleep(0.01)

        continue

    audio_window = get_audio_window()

    if audio_window is None:

        continue

    # =============================================
    # YAMNet
    # =============================================

    print("[YAMNet] inference running...")  

    scores_np, embeddings, spectrogram = run_yamnet(
        audio_window
    )

    # =============================================
    # Feature
    # =============================================

    (
        S_siren,
        S_horn,
        C_siren,
        C_horn
    ) = extract_features(scores_np)

    # =============================================
    # RMS Trend
    # =============================================

    update_rms_history(
        uart_handler.latest_rms_db
    )

    rms_trend = calculate_rms_trend()
    print(
    f"[TREND] RMS Trend = "
    f"{rms_trend:.3f}"
    )

    # =============================================
    # State Machine
    # =============================================

    level, label = update_state(
        S_siren,
        S_horn,
        C_siren,
        C_horn,
        uart_handler.latest_rms_db,
        uart_handler.latest_drms_db,
        rms_trend
    )

    # =============================================
    # Semantic Transition
    # =============================================

    update_state_history(label)

    transition = detect_semantic_transition()   

    # =============================================
    # Ducking
    # =============================================

    send_ducking(level)

    print(
        f"[STATE] {label} | "
        f"Level={level} | "
        f"Transition={transition}"
    )

    rms_trend = calculate_rms_trend()

    print(
        f"[TREND] RMS Trend = "
        f"{rms_trend:.3f}"
    )

    print(
    f"[ACOUSTIC] "
    f"RMS={uart_handler.latest_rms_db:.2f} dBFS | "
    f"dRMS={uart_handler.latest_drms_db:.2f} dB"
    )

    print(
        f"[SEMANTIC] "
        f"S_siren={S_siren:.3f} | "
        f"S_horn={S_horn:.3f}"
    )

    print(
        f"[CONFIDENCE] "
        f"C_siren={C_siren:.3f} | "
        f"C_horn={C_horn:.3f}"
    )

    print(
    f"[STATE] {label} | "
    f"Level={level} | "
    f"Transition={transition}"
    )

    loop_start = time.time()

    """
    UART 읽기
    ↓
    Trigger 확인
    ↓
    Sliding Window
    ↓
    YAMNet
    ↓
    Feature
    ↓
    Confidence
    ↓
    Trend
    ↓
    Transition
    ↓
    State Machine
    ↓
    Ducking

    즉, 실시간 semantic audio pipeline 전체를 담당

    Pi는 단순하는 sound classifier가 아니다
    ㅎ녀재 구조는 Context-awre Temporal Semantic decision system에 가깝다.

    즉, 현재 소리만 보는 것이 아니라 시간적으로 어떻게 변화하는가까지를 본다.
    """