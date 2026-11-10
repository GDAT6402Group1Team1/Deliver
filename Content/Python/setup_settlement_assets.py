"""生成结算逐行短音和总收益和弦，只更新 WBP_Reward 的声音配置

外部 Python 执行生成两条 WAV 和六行预听，UE 中执行则同时导入声音并配置
run(include_icon=True) 可额外导入美元图标，不改变结算布局
"""

import math
import struct
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WAVE_PATH = ROOT / "Design/Audio/GeneratedSFX/SFX_Settlement_Tone.wav"
FINAL_WAVE_PATH = ROOT / "Design/Audio/GeneratedSFX/SFX_Settlement_Finale.wav"
ICON_PATH = ROOT / "Content/UI/Image/T_Dollar.png"
SOUND_ASSET = "/Game/Sound/SFX/SFX_Settlement_Tone"
FINAL_SOUND_ASSET = "/Game/Sound/SFX/SFX_Settlement_Finale"
ICON_ASSET = "/Game/UI/Image/T_Dollar"
WIDGET_ASSET = "/Game/UI/Task/WBP_Reward"
SAMPLE_RATE = 48000
FREQUENCY = 261.6255653006  # C4，和弦与普通行共同随实际显示行数移调
DURATION = 0.34
FINAL_DURATION = 2.1
REVEAL_VOLUME = 1.0


def _note(channels, start, duration, frequency, gain, pan=0.0, sustain=0.09, detune=0.0):
    """带低频主体和明亮泛音的合成音，较长保持段避免退化成一次点击"""
    first = round(start * SAMPLE_RATE)
    count = min(round(duration * SAMPLE_RATE), len(channels[0]) - first)
    left_gain = math.sqrt((1.0 - pan) * 0.5)
    right_gain = math.sqrt((1.0 + pan) * 0.5)
    spread = 2 ** (detune / 1200.0)
    for index in range(count):
        time = index / SAMPLE_RATE
        phase = math.tau * frequency * time
        attack = math.sin(min(time / 0.009, 1.0) * math.pi / 2.0) ** 2
        release_seconds = max(duration - sustain, 0.02)
        release = max(0.0, 1.0 - max(time - sustain, 0.0) / release_seconds) ** 1.5
        tail = min((count - 1 - index) / (SAMPLE_RATE * 0.015), 1.0)
        envelope = attack * release * tail * gain
        # 以基音为主，保持音高清楚；高次泛音逐渐收敛，不使用刺耳方波
        tone = sum(weight * math.sin(phase * harmonic) for harmonic, weight in
                   ((1, 1.0), (2, 0.48), (3, 0.22), (4, 0.12), (5, 0.055)))
        left = 0.83 * tone + 0.17 * math.sin(phase / spread)
        right = 0.83 * tone + 0.17 * math.sin(phase * spread)
        channels[0][first + index] += envelope * left_gain * left
        channels[1][first + index] += envelope * right_gain * right


def _finish(channels, peak, room):
    """轻量早期反射和柔和饱和，尾部归零，保留输出余量"""
    dry = [channel[:] for channel in channels]
    for seconds, gain in room:
        delay = round(seconds * SAMPLE_RATE)
        for channel in range(2):
            for index in range(delay, len(channels[channel])):
                channels[channel][index] += gain * dry[1 - channel][index - delay]
    for channel in channels:
        for index, value in enumerate(channel):
            fade = min(index / (SAMPLE_RATE * 0.004),
                       (len(channel) - 1 - index) / (SAMPLE_RATE * 0.04), 1.0)
            channel[index] = math.tanh(value * 1.35) * fade
    maximum = max(abs(value) for channel in channels for value in channel)
    return [[value * peak / max(maximum, 1e-9) for value in channel] for channel in channels]


def _write(path, channels):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setparams((2, 2, SAMPLE_RATE, len(channels[0]), "NONE", "not compressed"))
        output.writeframes(b"".join(struct.pack("<hh", round(left * 32767), round(right * 32767))
                                   for left, right in zip(*channels)))
    print(f"[SettlementAssets] Generated {path}")


def generate_tone():
    """普通行使用八度叠层短音，总收益使用独立的大调六和弦与高音琶音"""
    rows = [[0.0] * round(SAMPLE_RATE * DURATION) for _ in range(2)]
    _note(rows, 0.0, 0.285, FREQUENCY, 0.75, sustain=0.11, detune=5)
    _note(rows, 0.0, 0.29, FREQUENCY / 2, 0.25, sustain=0.1)
    _note(rows, 0.004, 0.24, FREQUENCY * 2, 0.14, sustain=0.06, detune=3)
    rows = _finish(rows, 0.86, ((0.027, 0.12), (0.049, 0.07)))
    _write(WAVE_PATH, rows)

    final = [[0.0] * round(SAMPLE_RATE * FINAL_DURATION) for _ in range(2)]
    # 根音、五度、三度与六度展开到多个八度，基音随 Widget 移调后仍保持和谐
    for semitones, gain, pan in ((-12, 0.32, 0), (-5, 0.16, 0), (0, 0.48, -0.2),
                                 (4, 0.35, 0.45), (7, 0.32, -0.45), (9, 0.14, 0.6),
                                 (12, 0.23, -0.55), (16, 0.14, 0.3)):
        _note(final, 0.0, 1.75, FREQUENCY * 2 ** (semitones / 12), gain, pan,
              sustain=0.40, detune=6)
    # 总额出现时立刻响和弦，再用快速高音琶音拉出明亮的收尾
    for index, semitones in enumerate((12, 16, 19, 24)):
        _note(final, 0.045 + index * 0.055, 0.72,
              FREQUENCY * 2 ** (semitones / 12), 0.25, (-0.5, 0.25, -0.25, 0.5)[index],
              sustain=0.03, detune=2)
    final = _finish(final, 0.95, ((0.047, 0.16), (0.089, 0.13), (0.151, 0.09), (0.237, 0.06)))
    _write(FINAL_WAVE_PATH, final)
    return rows, final


def generate_preview(rows, final):
    """按默认显示时序生成预听，超出 PCM 范围时仅衰减预听，不改变 UE 音量"""
    mixed = [[0.0] * round(SAMPLE_RATE * 4.1) for _ in range(2)]
    for row, start in enumerate((0.2, 0.55, 0.9, 1.25, 1.6, 2.2)):
        pitch = 2 ** (row / 12)
        source = final if row == 5 else rows
        for index in range(int((len(source[0]) - 1) / pitch)):
            position = index * pitch
            low = int(position)
            fraction = position - low
            for channel in range(2):
                mixed[channel][round(start * SAMPLE_RATE) + index] += REVEAL_VOLUME * (
                    source[channel][low] * (1 - fraction) + source[channel][low + 1] * fraction)
    peak = max(abs(value) for channel in mixed for value in channel)
    if peak > 1.0:
        preview_gain = 0.98 / peak
        mixed = [[value * preview_gain for value in channel] for channel in mixed]
        print(f"[SettlementAssets] Preview-only gain {preview_gain:.3f}; UE volume remains {REVEAL_VOLUME}")
    _write(ROOT / "Saved/AudioPreview/SettlementScaleRich.wav", mixed)


def run(configure_widget=True, include_icon=False):
    rows, final = generate_tone()
    generate_preview(rows, final)
    try:
        import unreal
    except ImportError:
        return

    def import_asset(source, destination, factory):
        task = unreal.AssetImportTask()
        task.filename = str(source)
        task.destination_path, task.destination_name = destination.rsplit("/", 1)
        task.automated = True
        task.replace_existing = True
        task.save = True
        task.factory = factory
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        produced = list(task.get_editor_property("imported_object_paths"))
        asset = unreal.load_asset(destination)
        if not produced or not asset:
            raise RuntimeError(f"Import failed: {destination}, produced={produced}")
        return asset

    sounds = []
    for source, destination in ((WAVE_PATH, SOUND_ASSET), (FINAL_WAVE_PATH, FINAL_SOUND_ASSET)):
        sound = import_asset(source, destination, unreal.SoundFactory())
        sound.set_editor_property("looping", False)
        assert unreal.EditorAssetLibrary.save_loaded_asset(sound, only_if_is_dirty=False)
        sounds.append(sound)

    if include_icon:
        if not ICON_PATH.exists():
            raise FileNotFoundError(ICON_PATH)
        texture = import_asset(ICON_PATH, ICON_ASSET, unreal.TextureFactory())
        texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        texture.set_editor_property("srgb", True)
        texture.set_editor_property("never_stream", True)
        assert unreal.EditorAssetLibrary.save_loaded_asset(texture, only_if_is_dirty=False)

    if configure_widget:
        # 保留布局与显示间隔，只替换这次请求涉及的声音和音阶配置
        blueprint = unreal.load_asset(WIDGET_ASSET)
        widget_class = unreal.EditorAssetLibrary.load_blueprint_class(WIDGET_ASSET)
        defaults = unreal.get_default_object(widget_class)
        if not isinstance(defaults, unreal.DeliverySettlementWidget):
            raise RuntimeError("WBP_Reward must inherit DeliverySettlementWidget")
        defaults.set_editor_property("row_reveal_sound", sounds[0])
        defaults.set_editor_property("final_reveal_sound", sounds[1])
        defaults.set_editor_property("reveal_volume", REVEAL_VOLUME)
        defaults.set_editor_property("row_pitch_step", 1.0)
        defaults.set_editor_property("final_reveal_pitch", 1.0)
        unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
        assert unreal.EditorAssetLibrary.save_loaded_asset(blueprint, only_if_is_dirty=False)
    unreal.log("[SettlementAssets] Imported layered row tone and harmonized finale")


if __name__ == "__main__":
    run()
