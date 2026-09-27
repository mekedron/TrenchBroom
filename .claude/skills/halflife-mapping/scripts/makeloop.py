#!/usr/bin/env python3
"""Cuts a seamless loop of whole bars from a music WAV and writes a GoldSrc-ready WAV:
16-bit PCM mono with a cue point at sample 0 (ambient_generic loops a WAV only if it has a
cue point). The seam is crossfaded with the audio that follows the loop end.

Usage: makeloop.py SRC.wav OUT.wav --bpm 154 --start-bar 136 --bars 16 [--rate 44100]
Bars are counted from the first sample; check the phrase boundaries with --analyze first.
       makeloop.py SRC.wav - --bpm 154 --analyze      per-bar RMS and low/high band share
"""
import argparse
import struct
import wave

import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--bpm", type=float, required=True)
    ap.add_argument("--start-bar", type=int, default=0)
    ap.add_argument("--bars", type=int, default=16)
    ap.add_argument("--rate", type=int, default=0, help="resample to this rate (default: keep)")
    ap.add_argument("--crossfade-ms", type=float, default=30)
    ap.add_argument("--analyze", action="store_true")
    args = ap.parse_args()

    w = wave.open(args.src)
    sr, ch = w.getframerate(), w.getnchannels()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, ch).astype(np.float64) / 32768
    mono = x.mean(1)
    bar = 4 * 60 / args.bpm * sr

    if args.analyze:
        for b in range(int(len(mono) // bar)):
            seg = mono[int(b * bar):int((b + 1) * bar)]
            sp = np.abs(np.fft.rfft(seg))
            f = np.fft.rfftfreq(len(seg), 1 / sr)
            print(f"bar {b:3} t={b * bar / sr:6.1f}s rms={np.sqrt((seg ** 2).mean()):.3f} "
                  f"low={sp[f < 150].sum() / sp.sum():.2f} high={sp[f > 4000].sum() / sp.sum():.2f}")
        return

    s, e = int(round(args.start_bar * bar)), int(round((args.start_bar + args.bars) * bar))
    loop = mono[s:e].copy()
    n = int(args.crossfade_ms / 1000 * sr)
    t = np.linspace(0, np.pi / 2, n)
    loop[:n] = mono[e:e + n] * np.cos(t) + mono[s:s + n] * np.sin(t)
    rate = sr
    if args.rate and args.rate != sr:
        rate = args.rate
        loop = np.interp(np.arange(0, len(loop), sr / rate), np.arange(len(loop)), loop)
    loop *= 10 ** (-1 / 20) / np.abs(loop).max()
    pcm = (loop * 32767).astype("<i2").tobytes()

    def chunk(tag, data):
        return tag + struct.pack("<I", len(data)) + data + (b"\0" if len(data) % 2 else b"")

    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    cue = struct.pack("<I", 1) + struct.pack("<II4sIII", 0, 0, b"data", 0, 0, 0)
    body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"cue ", cue) + chunk(b"data", pcm)
    open(args.out, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
    print(f"{args.out}: {len(loop) / rate:.2f}s, {rate} Hz mono")


if __name__ == "__main__":
    main()
