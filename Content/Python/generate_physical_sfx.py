# -*- coding: utf-8 -*-
"""生成脚步（吸盘感）和出拳（挥空/命中）的占位音效，纯合成、不含第三方采样。

离线重生成 WAV：
    python Content/Python/generate_physical_sfx.py

在编辑器 Python 控制台里重生成 + 导入 + 绑定：
    import importlib, generate_physical_sfx as s; importlib.reload(s); s.run()

—— 为什么是合成而不是找素材 ——

要的是《猛兽派对》《舒舒服服小镇时光》那种**吸盘 / 果冻**质感，这类声音的共同点是
**音高本身在动**（一个短促的上滑或下滑），而不是靠音色复杂。真实录音里"脚踩地"
几乎都是宽频噪声、没有这个滑音，所以找素材反而更难对上；而滑音用合成最容易精确控制。

—— 这套音色由三层叠成 ——

  1. **噪声音头**（约 20ms）—— "啵"的那一下接触感。没有它纯滑音听着像乐器而不像踩地。
  2. **滑音正弦** —— 吸盘的主体。向上滑 = 吸盘脱离（拔起来），向下滑 = 吸住。
     脚步用**先下后上**的小弧：落地压下去、再弹一点，这是"软"的来源。
  3. **低频咚** —— 体重。频率很低、衰减极快，单独听几乎只是"噗"一下。

衰减一律用指数而不是线性：线性衰减的尾巴听着是被"切断"的，指数才像东西自己停下来。
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

# 脚步变体数量。8 个足够：C++ 侧会避开"上一次刚放过的那个"，
# 实际听到的最短重复间隔是 2 步，走起来不会察觉在循环。
FOOTSTEP_VARIANTS = 8


# ───────────────────────── 基础工具 ─────────────────────────

def _buffer(seconds: float) -> list[float]:
    return [0.0] * int(round(seconds * SAMPLE_RATE))


def _exp_decay(t: float, tau: float) -> float:
    """指数衰减。tau 是衰减到 1/e 的时间。"""
    return math.exp(-t / max(tau, 1e-5))


def _glide(
    samples: list[float],
    start: float,
    duration: float,
    freq_start: float,
    freq_end: float,
    amp: float,
    tau: float,
    *,
    bend: float = 1.0,
    attack: float = 0.004,
    harmonic2: float = 0.0,
) -> None:
    """一段滑音正弦。

    **相位必须累加，不能用 sin(2*pi*f(t)*t)。** 后者在频率变化时相位会跳，
    结果是滑音里夹着噼啪声——而且因为只在频率变化快的地方出现，
    听起来很像"素材本身有爆音"，极难归因。

    bend < 1 时滑音前段快后段慢（像被什么拽住），> 1 时相反。
    """
    begin = int(start * SAMPLE_RATE)
    count = int(duration * SAMPLE_RATE)
    phase = 0.0
    for i in range(count):
        index = begin + i
        if index >= len(samples):
            break
        t = i / SAMPLE_RATE
        progress = (t / duration) ** bend if duration > 0 else 1.0
        freq = freq_start + (freq_end - freq_start) * progress
        phase += 2.0 * math.pi * freq / SAMPLE_RATE
        env = _exp_decay(t, tau) * min(t / max(attack, 1e-5), 1.0)
        value = math.sin(phase)
        if harmonic2:
            # 二次谐波让音色从"纯正弦的玩具音"偏向"有腔体的橡胶音"。
            value += harmonic2 * math.sin(2.0 * phase)
        samples[index] += amp * env * value


def _noise_burst(
    samples: list[float],
    start: float,
    duration: float,
    amp: float,
    *,
    center: float = 2200.0,
    q: float = 1.2,
    seed: int = 0,
) -> None:
    """带通噪声音头。用双极点状态变量滤波器，固定中心频率。"""
    rng = random.Random(seed)
    begin = int(start * SAMPLE_RATE)
    count = int(duration * SAMPLE_RATE)
    f = 2.0 * math.sin(math.pi * min(center, SAMPLE_RATE * 0.45) / SAMPLE_RATE)
    damp = min(1.0 / max(q, 0.05), 1.9)
    low = band = 0.0
    for i in range(count):
        index = begin + i
        if index >= len(samples):
            break
        t = i / SAMPLE_RATE
        drive = rng.uniform(-1.0, 1.0)
        low += f * band
        high = drive - low - damp * band
        band += f * high
        samples[index] += amp * band * _exp_decay(t, duration * 0.3)


def _swept_noise(
    samples: list[float],
    start: float,
    duration: float,
    amp: float,
    center_start: float,
    center_end: float,
    *,
    q: float = 2.4,
    tau: float | None = None,
    seed: int = 0,
) -> None:
    """中心频率在扫动的带通噪声。挥拳的"呼"声全靠这个。

    **扫动是必须的**：固定带通的噪声听着是"嘶——"（像漏气），
    中心频率先升后降才有"东西从耳边掠过"的多普勒感。
    """
    rng = random.Random(seed)
    begin = int(start * SAMPLE_RATE)
    count = int(duration * SAMPLE_RATE)
    low = band = 0.0
    damp = min(1.0 / max(q, 0.05), 1.9)
    tau = duration * 0.45 if tau is None else tau
    for i in range(count):
        index = begin + i
        if index >= len(samples):
            break
        t = i / SAMPLE_RATE
        progress = t / duration if duration > 0 else 1.0
        # 先升后降：0→1→0 的半个正弦，顶点在中段
        shape = math.sin(math.pi * progress)
        center = center_start + (center_end - center_start) * shape
        f = 2.0 * math.sin(math.pi * min(center, SAMPLE_RATE * 0.45) / SAMPLE_RATE)
        drive = rng.uniform(-1.0, 1.0)
        low += f * band
        high = drive - low - damp * band
        band += f * high
        samples[index] += amp * band * shape * _exp_decay(t, tau)


# ───────────────────────── 脚步 ─────────────────────────

def _footstep(variant: int):
    """一个吸盘感脚步。

    变体之间**只动几个数值、不换结构**：换结构会让某几步听着像换了鞋，
    而真实的脚步变化只是落地角度和力度不同。
    """
    def build() -> list[float]:
        rng = random.Random(1000 + variant)
        samples = _buffer(0.26)

        # —— 基频按变体序号**均匀铺开**，不靠随机 ——
        #
        # 第一版是 `rng.uniform(-0.14, 0.14)`，实测 8 个变体里**撞出两对**
        # （03/04 波形相关度 0.958、05/06 是 0.931），听起来就是同一个音效。
        # 8 个样本在一个连续区间里随机取，出现两个挨得很近是常事而不是意外。
        # 均匀铺开才有"任意两个都分得开"的保证，再叠一点小抖动避免听出是等分的。
        #
        # 总跨度压到 ±1.3 个半音（±0.11 个八度）：跨得太大会变成"音阶"，
        # 走起来像在弹琴而不是走路。
        spread = -0.11 + 0.22 * variant / max(FOOTSTEP_VARIANTS - 1, 1)
        base = 196.0 * (2.0 ** (spread + rng.uniform(-0.012, 0.012)))

        # —— 音色参数按**另一个顺序**铺 ——
        #
        # 和基频同序铺的话，音高相邻的两个变体音色也相邻，等于白铺。
        # 乘 5 取模（5 与 8 互质）把顺序打散，于是音高接近的两个在音色上一定隔得远。
        timbre = (variant * 5) % FOOTSTEP_VARIANTS / max(FOOTSTEP_VARIANTS - 1, 1)

        # 主体：先往下压一点、再弹回去一点。
        # 这个"下-上"的小弧是软质吸盘的来源；只向上滑是"拔塞子"，
        # 只向下滑是"闷响"，两者都不够可爱。
        dip = base * (0.70 + 0.12 * timbre)
        _glide(samples, 0.0, 0.11, base * 1.06, dip, 0.62,
               tau=0.038 + 0.014 * timbre, bend=0.65,
               harmonic2=0.10 + 0.14 * timbre)
        _glide(samples, 0.045, 0.14, dip, base * (0.94 + 0.18 * (1.0 - timbre)), 0.40,
               tau=0.060, bend=1.5, harmonic2=0.10)

        # 噪声音头：接触的"啵"。中心频率也按 timbre 铺，模拟落地角度不同。
        _noise_burst(samples, 0.0, 0.020, 0.26 + 0.08 * timbre,
                     center=1650.0 + 1350.0 * timbre, q=1.1, seed=variant * 7 + 1)

        # 低频"咚"：体重落地。
        #
        # **这一层也必须跟着变体变。** 第一版它在 8 个变体里是同一组参数
        # （92→62Hz、同音量同衰减），而它是低频段最响的东西，于是**任意两个变体
        # 都因为这一层强相关**——实测两两相关度被顶到 0.85~0.87，光加大音高跨度
        # 一点用都没有（试过 ±1.6 个半音，相似度反而升到 0.872）。
        # 真实的脚步本来也是每步落地力度不同，变着才对。
        weight = (variant * 3) % FOOTSTEP_VARIANTS / max(FOOTSTEP_VARIANTS - 1, 1)
        _glide(samples, 0.002, 0.06 + 0.03 * weight,
               78.0 + 32.0 * weight, 52.0 + 22.0 * weight,
               0.26 + 0.16 * weight, tau=0.020 + 0.016 * weight, bend=0.8)

        # 尾部的小"啾"：吸盘脱离。**音量要很小**（0.10）——
        # 大了就从"脚底板吸一下"变成"踩到玩具"，每一步都抢戏。
        # 一半向上滑（吸盘"拔"出来）、一半向下滑（吸住后回落），
        # 按 timbre 连续过渡而不是只分奇偶：奇偶只有两种尾巴，
        # 撞上基频也接近时就是前面那两对"听着一样"的直接原因。
        if timbre < 0.5:
            _glide(samples, 0.085, 0.085, base * (1.9 + 0.5 * timbre * 2),
                   base * (3.2 - 0.4 * timbre * 2), 0.10, tau=0.030, bend=1.8)
        else:
            tail = (timbre - 0.5) * 2
            _glide(samples, 0.080, 0.090, base * (2.4 + 0.5 * tail),
                   base * (2.0 - 0.3 * tail), 0.09, tau=0.034, bend=1.2)

        return samples

    return build


# ───────────────────────── 出拳 ─────────────────────────

def _punch_swing_left() -> list[float]:
    """挥空的"呼"。左拳稍低、稍长，和右拳听得出区别。"""
    samples = _buffer(0.30)
    _swept_noise(samples, 0.0, 0.20, 0.52, 480.0, 2100.0, q=2.2, seed=11)
    # 一点点低频气流，给"手臂很沉"的感觉
    _swept_noise(samples, 0.01, 0.17, 0.22, 180.0, 620.0, q=3.0, seed=12)
    return samples


def _punch_swing_right() -> list[float]:
    samples = _buffer(0.30)
    _swept_noise(samples, 0.0, 0.175, 0.52, 620.0, 2600.0, q=2.2, seed=21)
    _swept_noise(samples, 0.01, 0.15, 0.20, 220.0, 740.0, q=3.0, seed=22)
    return samples


def _punch_hit() -> list[float]:
    """打中人的"砰"。卡通化：低频闷响 + 中频拍击 + 一丝弹性余音。

    **三层的比例是关键**：只有低频是"撞墙"，只有中频噪声是"拍手"，
    只有弹性余音是"玩具锤"。三层按 1 : 0.6 : 0.2 叠起来才像"揍到软的东西"。
    """
    samples = _buffer(0.34)
    # 低频闷响：主体重量。下滑让它听着像"陷进去"而不是敲钟。
    _glide(samples, 0.0, 0.16, 150.0, 70.0, 0.70, tau=0.040, bend=0.6, harmonic2=0.18)
    # 中频拍击：皮肉接触。短、宽、立刻衰减。
    _noise_burst(samples, 0.0, 0.055, 0.42, center=950.0, q=0.8, seed=31)
    _noise_burst(samples, 0.004, 0.035, 0.26, center=2600.0, q=1.4, seed=32)
    # 弹性余音：被打的人"颤"了一下。音量压到 0.14，是点睛不是主角。
    _glide(samples, 0.045, 0.19, 300.0, 215.0, 0.14, tau=0.065, bend=1.3)
    return samples


GENERATORS: dict = {
    "SFX_Footstep_%02d" % (i + 1): _footstep(i) for i in range(FOOTSTEP_VARIANTS)
}
GENERATORS.update({
    "SFX_Punch_Swing_L": _punch_swing_left,
    "SFX_Punch_Swing_R": _punch_swing_right,
    "SFX_Punch_Hit": _punch_hit,
})


# ───────────────────────── 写盘 ─────────────────────────

def _write(name: str, samples: list[float]) -> Path:
    SOURCE_DIR.mkdir(parents=True, exist_ok=True)

    # 轻微软削波：合成的瞬态峰值很尖，直接归一化会让整体音量偏小。
    shaped = [math.tanh(s * 1.05) for s in samples]
    peak = max((abs(s) for s in shaped), default=1.0)
    gain = 0.80 / max(peak, 1e-6)

    rng = random.Random(name)
    frames = bytearray()
    for value in shaped:
        # 一个 LSB 的三角抖动，避免指数尾巴量化成颗粒状的台阶。
        dither = (rng.random() - rng.random()) / 32767.0
        pcm = round(max(-1.0, min(1.0, value * gain + dither)) * 32767)
        frames += struct.pack("<h", pcm)

    # **单声道。** 这些都是空间音（PlaySoundAtLocation），
    # 立体声素材在 3D 衰减下会被引擎折叠，等于白占一倍体积。
    path = SOURCE_DIR / ("%s.wav" % name)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(bytes(frames))
    return path


def _has_property(obj, name: str) -> bool:
    try:
        obj.get_editor_property(name)
        return True
    except Exception:
        return False


def _import_into_unreal(paths: list[Path]) -> None:
    try:
        import unreal
    except ImportError:
        # **这里绝不能静默 return。** 命令行跑的时候只会生成 WAV、不会导入，
        # 而 run() 末尾那串 "[SFX] xxx.wav" 看着就像一切都成了——
        # 实际 /Game/Sound/SFX 里一个 uasset 都没多。这条就是被这个坑过一次才加的。
        print("[SFX] !! 不在编辑器里，只生成了 WAV，**没有导入 Unreal**。")
        print("[SFX]    要导入请在编辑器控制台（Cmd 模式）跑：py generate_physical_sfx.py")

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

    footsteps = [unreal.load_asset("%s/SFX_Footstep_%02d" % (DESTINATION, i + 1))
                 for i in range(FOOTSTEP_VARIANTS)]
    footsteps = [item for item in footsteps if item]
    if len(footsteps) != FOOTSTEP_VARIANTS:
        unreal.log_warning(
            "[SFX] 只载到 %d/%d 个脚步变体，剩下的绑定仍会继续，但重复会更明显。"
            % (len(footsteps), FOOTSTEP_VARIANTS))

    # —— 绑定到蓝图 CDO ——
    #
    # **不建 Sound Cue。** 随机挑变体那件事 C++ 侧已经做了（而且顺带避开了
    # 上一次刚放过的那个，Cue 的 Random 节点要手勾 "no repeats" 才有这个行为）。
    # 多一个 Cue 资产就多一处要维护、也多一处能填错。
    bindings = {
        "/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan": {
            "ActiveRagdoll": {"footstep_sounds": footsteps},
            "RagdollCombat": {
                "left_punch_sound": unreal.load_asset("%s/SFX_Punch_Swing_L" % DESTINATION),
                "right_punch_sound": unreal.load_asset("%s/SFX_Punch_Swing_R" % DESTINATION),
                "punch_hit_sound": unreal.load_asset("%s/SFX_Punch_Hit" % DESTINATION),
            },
        },
    }

    for blueprint_path, components in bindings.items():
        blueprint = unreal.load_asset(blueprint_path)
        if not blueprint:
            unreal.log_warning("[SFX] 找不到蓝图，未自动绑定：%s" % blueprint_path)
            continue
        default_object = unreal.get_default_object(blueprint.generated_class())
        for component_name, properties in components.items():
            component = (default_object.get_editor_property(component_name)
                         if _has_property(default_object, component_name) else None)
            if not component:
                unreal.log_warning(
                    "[SFX] %s 上取不到组件 %s，这几项要在蓝图详情面板里手填。"
                    % (blueprint_path, component_name))
                continue
            for property_name, value in properties.items():
                try:
                    component.set_editor_property(property_name, value)
                except Exception as error:
                    unreal.log_warning(
                        "[SFX] 写 %s.%s 失败：%s" % (component_name, property_name, error))
                    continue
                # set_editor_property 失败时不一定抛异常（枚举和受保护属性那几次都是静默的），
                # "调用过了"证明不了"写进去了"。回读是唯一的凭据。
                back = component.get_editor_property(property_name)
                unreal.log("[SFX] 回读 %s.%s = %s" % (component_name, property_name, back))
        unreal.EditorAssetLibrary.save_loaded_asset(blueprint, only_if_is_dirty=False)


def _check_variants_differ(threshold: float = 0.85) -> None:
    """脚步变体两两比波形相关度，太像就报错。

    **这个自检是必须的**，因为"听着一样"在报告里完全看不出来：
    第一版 8 个变体的基频是 `rng.uniform` 取的，撞出两对相关度 0.95+，
    而脚本照样打印"已写 8 个文件"、每个文件的峰值和 RMS 也都正常。
    加大音高跨度也骗不过它——试过把跨度从 ±1.2 提到 ±1.6 个半音，
    相似度反而从 0.854 升到 0.872，直接否掉了"音高不够散"这个猜测，
    真正的原因才浮出来（低频咚那一层在所有变体里是同一组参数）。
    """
    waves = []
    for index in range(FOOTSTEP_VARIANTS):
        path = SOURCE_DIR / ("SFX_Footstep_%02d.wav" % (index + 1))
        with wave.open(str(path), "rb") as handle:
            count = handle.getnframes()
            rate = handle.getframerate()
            data = struct.unpack("<%dh" % count, handle.readframes(count))
        # 只比前 120ms：后面是指数尾巴，所有变体都趋近于零，
        # 带上会把相关度统一拉高、把真正的差别冲淡。
        waves.append([v / 32768.0 for v in data[:int(0.12 * rate)]])

    worst = 0.0
    worst_pair = (0, 0)
    for i in range(FOOTSTEP_VARIANTS):
        for j in range(i + 1, FOOTSTEP_VARIANTS):
            a, b = waves[i], waves[j]
            na = math.sqrt(sum(v * v for v in a))
            nb = math.sqrt(sum(v * v for v in b))
            corr = abs(sum(p * q for p, q in zip(a, b)) / max(na * nb, 1e-9))
            if corr > worst:
                worst, worst_pair = corr, (i + 1, j + 1)

    print("[SFX] 脚步变体最高相似度 %.3f（%02d / %02d）" % (worst, *worst_pair))
    if worst > threshold:
        raise AssertionError(
            "脚步变体 %02d 和 %02d 波形相关度 %.3f > %.2f，玩家会听出是同一个音效。"
            % (worst_pair[0], worst_pair[1], worst, threshold))


def import_only() -> list[str]:
    """只导入已经生成好的 WAV，不重新合成。

    **在编辑器里优先用这个。** 合成是纯 Python 的逐样本循环
    （11 个文件 × 约 1.2 万~1.6 万个采样点），而编辑器的 Python 跑在**游戏线程**上，
    整个编辑器会卡住几十秒——看着像死了。WAV 已经在 `Design/Audio/GeneratedSFX`
    里了，改音色才需要重新合成（那时候在命令行跑 `run()`，卡的是终端不是编辑器）。
    """
    paths = [SOURCE_DIR / ("%s.wav" % name) for name in GENERATORS]
    missing = [p for p in paths if not p.exists()]
    if missing:
        print("[SFX] !! 少了 %d 个 WAV，先在命令行跑一次 run() 生成：" % len(missing))
        for path in missing:
            print("[SFX]    %s" % path)

        return []

    _import_into_unreal(paths)

    return [str(path) for path in paths]


def run() -> list[str]:
    paths = [_write(name, generator()) for name, generator in GENERATORS.items()]
    _check_variants_differ()
    _import_into_unreal(paths)
    for path in paths:
        print("[SFX] %s" % path)
    return [str(path) for path in paths]


if __name__ == "__main__":
    # **编辑器里 `py 脚本.py` 走的就是这条。** 默认只导入、不重新合成，
    # 否则编辑器会在游戏线程上卡几十秒。要重新合成就在命令行跑（见 import_only 的注释）。
    try:
        import unreal  # noqa: F401
        import_only()
    except ImportError:
        run()
