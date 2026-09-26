import argparse
import array
import json
import math
import sys
import wave
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze a PCM16 microphone WAVE capture")
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()

    with wave.open(str(arguments.capture), "rb") as capture:
        channels = capture.getnchannels()
        sample_rate = capture.getframerate()
        sample_width = capture.getsampwidth()
        frame_count = capture.getnframes()
        payload = capture.readframes(frame_count)
    if channels != 1 or sample_width != 2 or sample_rate <= 0:
        raise SystemExit("expected mono PCM16 WAVE input")

    samples = array.array("h")
    samples.frombytes(payload)
    if sys.byteorder != "little":
        samples.byteswap()
    if not samples:
        raise SystemExit("capture contains no samples")

    peak = 0
    sum_samples = 0
    sum_squares = 0
    clipped = 0
    maximum_step = 0
    large_steps = 0
    zero_run = 0
    longest_zero_run = 0
    previous = samples[0]
    for sample in samples:
        magnitude = abs(sample)
        peak = max(peak, magnitude)
        sum_samples += sample
        sum_squares += sample * sample
        clipped += magnitude >= 32_760
        step = abs(sample - previous)
        maximum_step = max(maximum_step, step)
        large_steps += step >= 16_384
        previous = sample
        if sample == 0:
            zero_run += 1
            longest_zero_run = max(longest_zero_run, zero_run)
        else:
            zero_run = 0

    window_frames = max(1, sample_rate // 10)
    silent_windows = 0
    minimum_window_rms = float("inf")
    maximum_window_rms = 0.0
    silence_threshold = 10 ** (-60.0 / 20.0)
    for begin in range(0, len(samples), window_frames):
        window = samples[begin:begin + window_frames]
        if len(window) < window_frames // 2:
            continue
        square_sum = sum(sample * sample for sample in window)
        rms = math.sqrt(square_sum / len(window)) / 32_768.0
        minimum_window_rms = min(minimum_window_rms, rms)
        maximum_window_rms = max(maximum_window_rms, rms)
        silent_windows += rms < silence_threshold

    rms = math.sqrt(sum_squares / len(samples)) / 32_768.0
    result = {
        "format": {
            "channels": channels,
            "sample_rate": sample_rate,
            "sample_width_bits": sample_width * 8,
            "frame_count": frame_count,
            "duration_seconds": round(frame_count / sample_rate, 6),
        },
        "signal": {
            "peak_amplitude": peak / 32_768.0,
            "peak_dbfs": None if peak == 0 else 20.0 * math.log10(peak / 32_768.0),
            "rms_amplitude": rms,
            "rms_dbfs": None if rms == 0 else 20.0 * math.log10(rms),
            "dc_offset": sum_samples / len(samples) / 32_768.0,
            "clipped_samples": clipped,
            "maximum_adjacent_step": maximum_step / 32_768.0,
            "large_adjacent_steps": large_steps,
        },
        "continuity_indicators": {
            "analysis_window_milliseconds": 100,
            "silent_window_threshold_dbfs": -60,
            "silent_windows": silent_windows,
            "minimum_window_rms_dbfs": None if minimum_window_rms == 0 else
                20.0 * math.log10(minimum_window_rms),
            "maximum_window_rms_dbfs": None if maximum_window_rms == 0 else
                20.0 * math.log10(maximum_window_rms),
            "longest_exact_zero_run_milliseconds":
                longest_zero_run * 1_000.0 / sample_rate,
        },
    }
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    print(encoded)
    if arguments.output:
        arguments.output.write_text(encoded + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
