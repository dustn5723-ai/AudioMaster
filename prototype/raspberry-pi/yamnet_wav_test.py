#!/usr/bin/env python3
import os
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import argparse
import csv
import glob
import wave

import numpy as np
import tensorflow as tf
import tensorflow_hub as hub

MODEL_URL = "https://tfhub.dev/google/yamnet/1"

SIREN_NAMES = {
    "Siren",
    "Civil defense siren",
}
HORN_NAMES = {
    "Vehicle horn, car horn, honking",
    "Air horn, truck horn",
    "Train horn",
}

def load_class_names(model):
    p = model.class_map_path().numpy()
    if isinstance(p, bytes):
        p = p.decode("utf-8")
    names = []
    with tf.io.gfile.GFile(p) as f:
        for row in csv.DictReader(f):
            names.append(row["display_name"])
    return names

def load_wav(path):
    with wave.open(path, "rb") as w:
        ch = w.getnchannels()
        rate = w.getframerate()
        width = w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if ch != 1 or rate != 16000 or width != 2:
        raise RuntimeError(
            f"Expected mono/16kHz/16-bit WAV, got channels={ch}, rate={rate}, width={width}"
        )
    pcm = np.frombuffer(raw, dtype="<i2")
    waveform = pcm.astype(np.float32) / 32768.0
    return waveform

def group_indices(class_names, wanted):
    return [i for i, name in enumerate(class_names) if name in wanted]

def summarize(scores, class_names, indices):
    if not indices:
        return 0.0, 0.0, []
    arr = scores[:, indices]
    mean_score = float(np.max(np.mean(arr, axis=0)))
    peak_score = float(np.max(arr))
    detail = [(class_names[i], float(np.max(scores[:, i]))) for i in indices]
    detail.sort(key=lambda x: x[1], reverse=True)
    return mean_score, peak_score, detail

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", nargs="?", help="WAV file; default = newest ~/captures/*.wav")
    args = ap.parse_args()

    path = args.wav
    if not path:
        files = sorted(glob.glob(os.path.expanduser("~/captures/*.wav")))
        if not files:
            raise SystemExit("No WAV files found in ~/captures")
        path = files[-1]

    print("[LOAD MODEL]", MODEL_URL)
    model = hub.load(MODEL_URL)
    class_names = load_class_names(model)

    waveform = load_wav(path)
    duration = len(waveform) / 16000.0
    print(f"[WAV] {path}")
    print(f"[AUDIO] 16000 Hz / mono / float32 / {duration:.2f} s")

    scores, embeddings, spectrogram = model(tf.convert_to_tensor(waveform, tf.float32))
    scores = scores.numpy()

    mean_scores = scores.mean(axis=0)
    peak_scores = scores.max(axis=0)

    print("\n=== TOP 10 by PEAK score ===")
    for idx in np.argsort(peak_scores)[::-1][:10]:
        print(f"{class_names[idx]:45s} peak={peak_scores[idx]:.4f} mean={mean_scores[idx]:.4f}")

    siren_idx = group_indices(class_names, SIREN_NAMES)
    horn_idx = group_indices(class_names, HORN_NAMES)

    siren_mean, siren_peak, siren_detail = summarize(scores, class_names, siren_idx)
    horn_mean, horn_peak, horn_detail = summarize(scores, class_names, horn_idx)

    print("\n=== TARGETS ===")
    print(f"SIREN: peak={siren_peak:.4f}, mean={siren_mean:.4f}")
    for name, v in siren_detail:
        print(f"  {name}: peak={v:.4f}")

    print(f"HORN : peak={horn_peak:.4f}, mean={horn_mean:.4f}")
    for name, v in horn_detail:
        print(f"  {name}: peak={v:.4f}")

    print(f"\n[YAMNET] score_frames={scores.shape[0]}, classes={scores.shape[1]}")
    print("[OK] YAMNet inference completed.")

if __name__ == "__main__":
    main()
