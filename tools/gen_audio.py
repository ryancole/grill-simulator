#!/usr/bin/env python3
"""Generates the placeholder sound effects under assets/audio/.

Same bargain as gen_models.py: the game has no sound designer, so the sizzle is
synthesised from noise here rather than recorded. It is a stopgap -- replacing it
with a real .wav is a file copy, because the game just loads whatever WAV sits at
assets/audio/sizzle.wav.

Not wired into CMake on purpose. Building a C++ game should not need a Python
interpreter. Run it by hand and commit what it writes:

    python tools/gen_audio.py

Only the standard library is used: `wave` writes the container, `struct` packs
the samples. Everything is driven off a fixed seed so the committed .wav is
byte-for-byte reproducible.

The sizzle is a seamless loop
-----------------------------
The grill is always lit, so its sound plays on a loop forever. Because the source
is stationary filtered noise, the wrap from the last sample back to the first is
just one more random step -- indistinguishable from the noise either side of it --
so no crossfade is needed to hide the seam.
"""

from __future__ import annotations

import math
import pathlib
import struct
import wave

AUDIO = pathlib.Path(__file__).resolve().parent.parent / "assets" / "audio"

SAMPLE_RATE = 22050  # A hiss lives in the highs; 22 kHz carries it and halves the file.
DURATION = 2.0       # Seconds. Long enough that the loop is not obvious.
PEAK = 0.35          # Headroom below full scale so the loudest spit never clips.


class Lcg:
    """A tiny linear-congruential generator, so the output does not depend on the
    host's `random` implementation and the committed file never drifts."""

    def __init__(self, seed: int) -> None:
        self._state = seed & 0xFFFFFFFF

    def uniform(self) -> float:
        # Numerical Recipes' constants. Returns a float in [-1, 1).
        self._state = (self._state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self._state / 0x80000000 - 1.0


def _render(rng: Lcg, sample_count: int) -> list[float]:
    """A fat, spitting hiss: white noise, band-shaped into the sizzle register and
    slowly swelling, with the odd grease-pop riding on top."""
    out: list[float] = []

    # A one-pole high-pass takes the low rumble out (the difference of successive
    # samples), then a one-pole low-pass tames the harsh top. What survives is the
    # mid-high band a real sizzle occupies.
    hp_prev_in = 0.0
    hp_prev_out = 0.0
    lp = 0.0
    hp_coeff = 0.92
    lp_coeff = 0.45

    # A slow amplitude swell, plus a faster flutter, so the hiss breathes like fat
    # rendering rather than sitting at one dead level.
    pop_countdown = 400
    pop = 0.0

    for i in range(sample_count):
        white = rng.uniform()

        hp = hp_coeff * (hp_prev_out + white - hp_prev_in)
        hp_prev_in = white
        hp_prev_out = hp
        lp = lp + lp_coeff * (hp - lp)

        # Envelope: a 0.7 Hz swell around 0.8, jittered a little every sample.
        swell = 0.8 + 0.2 * math.sin(2.0 * math.pi * 0.7 * i / SAMPLE_RATE)
        flutter = 0.85 + 0.15 * rng.uniform()
        env = swell * flutter

        # Grease pops: every so often, kick off a short decaying transient.
        pop_countdown -= 1
        if pop_countdown <= 0:
            pop = 0.6 * rng.uniform()
            # Space the next pop irregularly, 150-550 samples out (~7-25 ms).
            pop_countdown = 150 + int(200 * (rng.uniform() + 1.0))
        pop *= 0.85  # Decay whatever pop is currently ringing.

        out.append(PEAK * (lp * env + pop))

    # Normalise to the peak we asked for; the filters make the raw level hard to
    # predict, and a loud-enough loop matters more than an exact one.
    loudest = max(1e-6, max(abs(s) for s in out))
    scale = PEAK / loudest
    return [s * scale for s in out]


def write_wav(path: pathlib.Path, samples: list[float]) -> None:
    frames = bytearray()
    for s in samples:
        clamped = max(-1.0, min(1.0, s))
        frames += struct.pack("<h", int(clamped * 32767.0))

    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)      # Mono: X3DAudio pans a point source; stereo would fight it.
        w.setsampwidth(2)      # 16-bit PCM, the format SoundEffect loads without fuss.
        w.setframerate(SAMPLE_RATE)
        w.writeframes(bytes(frames))


def main() -> None:
    AUDIO.mkdir(parents=True, exist_ok=True)
    count = int(SAMPLE_RATE * DURATION)
    write_wav(AUDIO / "sizzle.wav", _render(Lcg(0x512E1E), count))
    print(f"wrote {AUDIO / 'sizzle.wav'} ({count} frames, {DURATION:g}s)")


if __name__ == "__main__":
    main()
