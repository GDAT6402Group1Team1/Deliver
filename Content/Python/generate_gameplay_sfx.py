"""Generate the first stylized phone/task SFX pack and optionally import it into Unreal.

Run outside Unreal to regenerate WAV sources:
    python Content/Python/generate_gameplay_sfx.py

Run in Unreal's Python console to regenerate and import /Game/Sound/SFX:
    import importlib, generate_gameplay_sfx as sfx; importlib.reload(sfx); sfx.run()
"""

from __future__ import annotations

import math
import random
import struct
import wave
from pathlib import Path


SAMPLE_RATE = 48_000
PROJECT_ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = PROJECT_ROOT / "Design" / "Audio" / "GeneratedSFX"
DESTINATION = "/Game/Sound/SFX"


def _buffer(seconds: float) -> list[float]:
    return [0.0] * int(round(seconds * SAMPLE_RATE))


def _envelope(local_time: float, duration: float, attack: float, release: float) -> float:
    if local_time < 0.0 or local_time >= duration:
        return 0.0
    attack_gain = min(local_time / max(attack, 1e-5), 1.0)
    release_gain = min((duration - local_time) / max(release, 1e-5), 1.0)
    return max(0.0, min(attack_gain, release_gain))


def _tone(
    samples: list[float],
    start: float,
    duration: float,
    frequency: float,
    *,
    end_frequency: float | None = None,
    amplitude: float = 0.4,
    attack: float = 0.008,
    release: float = 0.05,
    harmonics: tuple[tuple[float, float], ...] = ((2.0, 0.18), (3.0, 0.06)),
    vibrato_hz: float = 0.0,
    vibrato_depth: float = 0.0,
    tremolo_hz: float = 0.0,
    tremolo_depth: float = 0.0,
) -> None:
    first = max(0, int(round(start * SAMPLE_RATE)))
    count = min(int(round(duration * SAMPLE_RATE)), len(samples) - first)
    phase = 0.0
    for index in range(count):
        local = index / SAMPLE_RATE
        alpha = local / max(duration, 1e-5)
        base = frequency if end_frequency is None else frequency + (end_frequency - frequency) * alpha
        instant = base * (2.0 ** ((math.sin(math.tau * vibrato_hz * local) * vibrato_depth) / 12.0))
        phase += math.tau * instant / SAMPLE_RATE
        value = math.sin(phase)
        for multiplier, strength in harmonics:
            value += math.sin(phase * multiplier) * strength
        tremolo = 1.0 - tremolo_depth * 0.5 + math.sin(math.tau * tremolo_hz * local) * tremolo_depth * 0.5
        samples[first + index] += value * amplitude * tremolo * _envelope(local, duration, attack, release)


def _noise_click(
    samples: list[float], start: float, duration: float, amplitude: float, seed: int
) -> None:
    rng = random.Random(seed)
    first = max(0, int(round(start * SAMPLE_RATE)))
    count = min(int(round(duration * SAMPLE_RATE)), len(samples) - first)
    filtered = 0.0
    for index in range(count):
        local = index / SAMPLE_RATE
        filtered += (rng.uniform(-1.0, 1.0) - filtered) * 0.22
        samples[first + index] += filtered * amplitude * _envelope(local, duration, 0.001, duration)


def _fm_tone(
    samples: list[float],
    start: float,
    duration: float,
    frequency: float,
    *,
    end_frequency: float | None = None,
    amplitude: float = 0.3,
    modulation_ratio: float = 2.01,
    modulation_index: float = 1.6,
    attack: float = 0.008,
    decay: float = 3.0,
    release: float = 0.08,
) -> None:
    """Warm two-operator FM with a detuned carrier and a decaying bright onset."""
    first = max(0, int(round(start * SAMPLE_RATE)))
    count = min(int(round(duration * SAMPLE_RATE)), len(samples) - first)
    carrier_phase = 0.0
    detuned_phase = 0.0
    mod_phase = 0.0
    for index in range(count):
        local = index / SAMPLE_RATE
        alpha = local / max(duration, 1e-5)
        base = frequency if end_frequency is None else frequency + (end_frequency - frequency) * alpha
        carrier_phase += math.tau * base / SAMPLE_RATE
        detuned_phase += math.tau * base * 1.0045 / SAMPLE_RATE
        mod_phase += math.tau * base * modulation_ratio / SAMPLE_RATE
        brightness = modulation_index * (0.30 + 0.70 * math.exp(-local * decay * 2.0))
        body = math.sin(carrier_phase + math.sin(mod_phase) * brightness)
        body += math.sin(detuned_phase + math.sin(mod_phase * 0.503) * brightness * 0.32) * 0.30
        body += math.sin(carrier_phase * 0.5) * 0.09
        shaped = _envelope(local, duration, attack, release) * math.exp(-local * decay)
        samples[first + index] += body * amplitude * shaped


def _bell(
    samples: list[float], start: float, duration: float, frequency: float, amplitude: float = 0.28
) -> None:
    """Inharmonic modal bell; partials decay at different rates instead of sounding like a saw wave."""
    first = max(0, int(round(start * SAMPLE_RATE)))
    count = min(int(round(duration * SAMPLE_RATE)), len(samples) - first)
    partials = ((1.0, 1.0, 2.8), (2.01, 0.34, 4.2), (2.74, 0.20, 5.1),
                (4.08, 0.11, 6.6), (5.43, 0.055, 8.0))
    phases = [0.0] * len(partials)
    for index in range(count):
        local = index / SAMPLE_RATE
        attack = min(local / 0.004, 1.0)
        value = 0.0
        for part_index, (ratio, strength, decay) in enumerate(partials):
            phases[part_index] += math.tau * frequency * ratio / SAMPLE_RATE
            value += math.sin(phases[part_index]) * strength * math.exp(-local * decay)
        release = min((duration - local) / 0.08, 1.0)
        samples[first + index] += value * amplitude * attack * max(0.0, release)


def _lowpass(samples: list[float], cutoff_hz: float) -> list[float]:
    coefficient = 1.0 - math.exp(-math.tau * cutoff_hz / SAMPLE_RATE)
    output: list[float] = []
    state = 0.0
    for sample in samples:
        state += (sample - state) * coefficient
        output.append(state)
    return output


def _stereo_master(samples: list[float], ambience: float) -> tuple[list[float], list[float]]:
    """Short asymmetric room taps make UI tones wider without a long washy reverb."""
    dry = _lowpass(samples, 10_500.0)
    left = list(dry)
    right = list(dry)
    taps = ((0.017, 0.16, 0.08), (0.031, 0.08, 0.15), (0.061, 0.055, 0.04))
    for seconds, left_gain, right_gain in taps:
        delay = int(seconds * SAMPLE_RATE)
        for index in range(delay, len(dry)):
            source = dry[index - delay] * ambience
            left[index] += source * left_gain
            right[index] += source * right_gain
    # A tiny opposite-side chorus delay gives width while remaining mono-compatible.
    chorus_delay = int(0.0065 * SAMPLE_RATE)
    for index in range(chorus_delay, len(dry)):
        right[index] += dry[index - chorus_delay] * 0.075
        left[index] -= dry[index - chorus_delay] * 0.025
    return left, right


def _write(name: str, samples: list[float]) -> Path:
    SOURCE_DIR.mkdir(parents=True, exist_ok=True)
    ambience = 0.75 if "Task" in name else 0.48
    left, right = _stereo_master(samples, ambience)
    # Gentle saturation and a lower target peak leave headroom for dialogue and simultaneous UI sounds.
    left = [math.tanh(sample * 0.92) for sample in left]
    right = [math.tanh(sample * 0.92) for sample in right]
    peak = max(max((abs(sample) for sample in left), default=1.0),
               max((abs(sample) for sample in right), default=1.0))
    gain = min(2.0, 0.68 / max(peak, 1e-6))
    rng = random.Random(name)
    frame_parts: list[bytes] = []
    for left_sample, right_sample in zip(left, right):
        # One-LSB triangular dither prevents quiet reverb tails from turning into grainy steps.
        dither = (rng.random() - rng.random()) / 32767.0
        left_pcm = round(max(-1.0, min(1.0, left_sample * gain + dither)) * 32767)
        right_pcm = round(max(-1.0, min(1.0, right_sample * gain - dither)) * 32767)
        frame_parts.append(struct.pack("<hh", left_pcm, right_pcm))
    frames = b"".join(frame_parts)
    path = SOURCE_DIR / f"{name}.wav"
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(frames)
    return path


def _phone_toggle() -> list[float]:
    samples = _buffer(0.30)
    _noise_click(samples, 0.0, 0.022, 0.10, 11)
    _fm_tone(samples, 0.012, 0.17, 610, end_frequency=970, amplitude=0.31,
             modulation_ratio=1.51, modulation_index=1.15, decay=4.8, release=0.07)
    _fm_tone(samples, 0.07, 0.16, 1210, end_frequency=910, amplitude=0.13,
             modulation_ratio=2.49, modulation_index=0.75, decay=5.5, release=0.07)
    return samples


def _phone_ring() -> list[float]:
    # Two phrases inside an exact two-second loop, with enough silence for the room tail to die out.
    samples = _buffer(2.0)
    for phrase_start in (0.10, 0.86):
        for note_start, frequency in ((0.00, 659.25), (0.19, 830.61), (0.38, 739.99)):
            _fm_tone(samples, phrase_start + note_start, 0.24, frequency,
                     amplitude=0.25, modulation_ratio=2.007, modulation_index=2.1,
                     decay=3.7, release=0.10)
            _tone(samples, phrase_start + note_start, 0.20, frequency * 0.5,
                  amplitude=0.055, attack=0.012, release=0.09, harmonics=())
    return samples


def _phone_answer() -> list[float]:
    samples = _buffer(0.48)
    _noise_click(samples, 0.0, 0.016, 0.08, 22)
    _fm_tone(samples, 0.012, 0.25, 493.88, end_frequency=530, amplitude=0.27,
             modulation_ratio=1.49, modulation_index=1.2, decay=4.2, release=0.12)
    _fm_tone(samples, 0.13, 0.29, 739.99, end_frequency=790, amplitude=0.29,
             modulation_ratio=2.01, modulation_index=1.55, decay=3.7, release=0.15)
    return samples


def _phone_hangup() -> list[float]:
    samples = _buffer(0.54)
    _fm_tone(samples, 0.00, 0.27, 740, end_frequency=570, amplitude=0.27,
             modulation_ratio=1.98, modulation_index=1.45, decay=4.0, release=0.12)
    _fm_tone(samples, 0.15, 0.32, 440, end_frequency=246.94, amplitude=0.30,
             modulation_ratio=1.51, modulation_index=1.7, decay=3.6, release=0.16)
    _noise_click(samples, 0.38, 0.035, 0.07, 33)
    return samples


def _task_pickup() -> list[float]:
    samples = _buffer(0.70)
    _noise_click(samples, 0.0, 0.022, 0.07, 44)
    _bell(samples, 0.018, 0.48, 587.33, 0.22)
    _bell(samples, 0.17, 0.48, 880.00, 0.25)
    _tone(samples, 0.18, 0.30, 440.0, amplitude=0.045, attack=0.02,
          release=0.18, harmonics=())
    return samples


def _task_delivered() -> list[float]:
    samples = _buffer(1.25)
    for index, frequency in enumerate((523.25, 659.25, 783.99, 1046.50)):
        _bell(samples, 0.04 + index * 0.14, 0.68, frequency,
              0.17 if index < 3 else 0.24)
    _fm_tone(samples, 0.49, 0.56, 523.25, end_frequency=536.0, amplitude=0.10,
             modulation_ratio=1.502, modulation_index=0.65, decay=2.8, release=0.25)
    _noise_click(samples, 0.02, 0.025, 0.055, 55)
    return samples


GENERATORS = {
    "SFX_Phone_Toggle": _phone_toggle,
    "SFX_Phone_Ring": _phone_ring,
    "SFX_Phone_Answer": _phone_answer,
    "SFX_Phone_HangUp": _phone_hangup,
    "SFX_Task_Pickup": _task_pickup,
    "SFX_Task_Delivered": _task_delivered,
}


def _import_into_unreal(paths: list[Path]) -> None:
    try:
        import unreal
    except ImportError:
        return

    tasks = []
    for path in paths:
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", str(path))
        task.set_editor_property("destination_path", DESTINATION)
        task.set_editor_property("destination_name", path.stem)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    ring = unreal.load_asset(f"{DESTINATION}/SFX_Phone_Ring")
    if ring:
        try:
            ring.set_editor_property("looping", True)
            unreal.EditorAssetLibrary.save_loaded_asset(ring, only_if_is_dirty=False)
        except Exception as error:
            unreal.log_warning(f"[SFX] 来电资产已导入，但 Looping 需要手动勾选：{error}")

    # These are deliberately only the phone/task defaults. Punch, grab and vehicle sounds
    # remain empty until suitable physical recordings are chosen.
    bindings = {
        "/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan": {
            "pickup_success_sound": f"{DESTINATION}/SFX_Task_Pickup",
            "delivery_success_sound": f"{DESTINATION}/SFX_Task_Delivered",
        },
        "/Game/Blueprint/PlayerController/BP_DeliveryManPC": {
            "phone_toggle_sound": f"{DESTINATION}/SFX_Phone_Toggle",
            "incoming_call_sound": f"{DESTINATION}/SFX_Phone_Ring",
            "answer_call_sound": f"{DESTINATION}/SFX_Phone_Answer",
            "hang_up_call_sound": f"{DESTINATION}/SFX_Phone_HangUp",
        },
    }
    for blueprint_path, properties in bindings.items():
        blueprint = unreal.load_asset(blueprint_path)
        if not blueprint:
            unreal.log_warning(f"[SFX] 找不到蓝图，未自动绑定：{blueprint_path}")
            continue
        try:
            default_object = unreal.get_default_object(blueprint.generated_class())
            for property_name, sound_path in properties.items():
                default_object.set_editor_property(property_name, unreal.load_asset(sound_path))
            unreal.EditorAssetLibrary.save_loaded_asset(blueprint, only_if_is_dirty=False)
            unreal.log(f"[SFX] 已绑定默认音效：{blueprint_path}")
        except Exception as error:
            unreal.log_warning(f"[SFX] 自动绑定失败 {blueprint_path}：{error}")


def run() -> list[str]:
    paths = [_write(name, generator()) for name, generator in GENERATORS.items()]
    _import_into_unreal(paths)
    for path in paths:
        print(f"[SFX] {path}")
    return [str(path) for path in paths]


if __name__ == "__main__":
    run()
