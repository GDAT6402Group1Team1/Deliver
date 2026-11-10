# 把电话通话时长配到"够念完台词"
#
# 通话时长是服务器计时、到点直接挂断，**不等本机念完**（刻意的：队列全局共享，
# 不能让某个客户端的播放进度决定所有人什么时候进下一通）。于是 DurationSeconds
# 配得比台词短，表现就是"NPC 话没说完就被挂断"——而那看着像对白写短了，
# 几乎不会让人想到是时长没配。
#
# **秒数由 C++ 算，脚本不自己抄计时规则。**
# UDeliveryDialogueVoiceComponent::EstimateSpeechSeconds 和真实播放共用同一个
# GlyphDelaySeconds，所以这里算出来的就是实际会念多久；而那条规则里有窄字提速、
# \r\n 合并、确定性抖动三件事，在 Python 里复刻一份迟早和 C++ 漂开。
#
# 脚本幂等：已经够长的不动，只把不够的调上去。不碰对白、来电人、铃声时长。
#
# 改完台词或者改了 NarrowGlyphScale / CharacterInterval 之后重跑一次即可。

import math

import unreal

TASKS = ["/Game/Task/Definitions/DA_Task_001"]
CALL_FIELDS = ["unlock_call", "overdue_call"]

# 每个字符用一次性计时器排下一个字，帧尾超出的时间不会补回来
# 原先只乘 1.35，在低帧率关卡仍可能提前挂断
# 按最低 10 FPS 给每个 UTF-16 字符预留一整帧，再留 2 秒阅读尾句
# 这是保守配置预算，不是实际播放时长；持续低于 10 FPS 仍需增大余量
MIN_PLAYBACK_FPS = 10.0
TAIL_SECONDS = 2.0


def _as_text(text):
    """FText 参数：str 自动转换和 unreal.Text() 两条路都试，别为这个卡住"""
    try:
        return unreal.Text(text)
    except Exception:
        return text


def estimate_seconds(text):
    """念完这段要几秒。走 C++，没有第二份实现"""
    return float(
        unreal.DeliveryDialogueVoiceComponent.estimate_speech_seconds(_as_text(text))
    )


def duration_budget(text):
    """返回理想播放秒数和含低帧率余量的配置秒数"""
    need = estimate_seconds(text)
    # UE 按 UTF-16 下标逐字播放，Python len 对补充平面字符的计数不同
    # CRLF 等零延时字符也留一帧，只会增加余量，不复制 C++ 的逐字规则
    glyph_count = len(text.encode("utf-16-le")) // 2
    required = need + glyph_count / MIN_PLAYBACK_FPS + TAIL_SECONDS
    # 必须向上取整，普通 round 可能把配置压回所需时长以下
    return need, math.ceil(required * 2) / 2.0


def spoken_prefix(text, seconds):
    """给定秒数能念到哪儿，用来指出截断位置。同样走 C++"""
    count = int(
        unreal.DeliveryDialogueVoiceComponent.spoken_glyph_count_within(
            _as_text(text), seconds
        )
    )

    return text[:count]


def check_cpp_available():
    """C++ 没编进来时要立刻报清楚，而不是让后面每一步都"莫名失败" """
    cls = getattr(unreal, "DeliveryDialogueVoiceComponent", None)
    if cls is None:
        return "unreal.DeliveryDialogueVoiceComponent 不存在"
    for name in ("estimate_speech_seconds", "spoken_glyph_count_within"):
        if not hasattr(cls, name):
            return f"{name} 不存在（C++ 改动还没编译进当前编辑器）"

    return None


def run():
    lines = []

    missing = check_cpp_available()
    if missing:
        msg = (
            f"[通话时长] 停止：{missing}。\n"
            "            先关编辑器、重编 DeliveryEditor 目标，再重开编辑器跑这个脚本。"
        )
        unreal.log_error(msg)

        return

    changed_assets = 0
    for path in TASKS:
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if asset is None:
            lines.append(f"[跳过] 加载不到 {path}")
            continue
        # load_asset 会把 ObjectRedirector 当成对象成功返回，所以要显式认一下：
        # 本仓库的蓝图被挪过目录，旧路径只剩重定向器，而那时所有后续 API 都会"全部失败"
        if asset.get_class().get_name() == "ObjectRedirector":
            lines.append(f"[跳过] {path} 是重定向器，不是任务资产")
            continue

        lines.append(f"\n=== {path} ===")
        asset_changed = False

        for field in CALL_FIELDS:
            try:
                call = asset.get_editor_property(field)
            except Exception as exc:
                lines.append(f"  [跳过] 读不到 {field}: {exc}")
                continue

            dialogue = str(call.get_editor_property("dialogue"))
            current = float(call.get_editor_property("duration_seconds"))

            if not dialogue.strip():
                lines.append(f"  {field:<14} 没有台词，不动（现 {current:.1f}s）")
                continue

            need, target = duration_budget(dialogue)

            if current >= target:
                lines.append(
                    f"  {field:<14} 现 {current:.1f}s >= 预算 {target:.1f}s"
                    f"（理想 {need:.1f}s + {MIN_PLAYBACK_FPS:g} FPS 余量 + 尾句停留），不动"
                )
                continue

            cut = spoken_prefix(dialogue, current)
            line_no = cut.count("\n") + 1
            call.set_editor_property("duration_seconds", target)
            asset.set_editor_property(field, call)
            asset_changed = True

            lines.append(
                f"  {field:<14} {current:.1f}s -> {target:.1f}s"
                f"（台词 {len(dialogue)} 字符、理想 {need:.1f}s；"
                f"另留 {MIN_PLAYBACK_FPS:g} FPS 余量和尾句停留；"
                f"原时长按理想节奏可到第 {line_no} 行 ...{cut[-24:]!r}，未计帧等待）"
            )

        if asset_changed:
            # 不存盘的话重启编辑器就全没了（本仓库踩过：写完立刻读回是好的，存盘重载变回默认值）
            if not unreal.EditorAssetLibrary.save_asset(path):
                raise RuntimeError(f"[通话时长] 保存失败：{path}")
            changed_assets += 1
            lines.append("  已保存")
        else:
            lines.append("  本来就都够长，没有改动")

    # 回读核对：计数器只能证明函数被调用过，证明不了结果留住了
    lines.append("\n=== 回读核对 ===")
    for path in TASKS:
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if asset is None:
            continue
        for field in CALL_FIELDS:
            try:
                call = asset.get_editor_property(field)
            except Exception:
                continue
            dialogue = str(call.get_editor_property("dialogue"))
            current = float(call.get_editor_property("duration_seconds"))
            if not dialogue.strip():
                continue
            need, required = duration_budget(dialogue)
            ok = "够" if current >= required else ">>> 仍然不够"
            lines.append(
                f"  {field:<14} {current:.1f}s / 需要 {required:.1f}s"
                f"（理想 {need:.1f}s + {MIN_PLAYBACK_FPS:g} FPS 余量 + 尾句停留）  {ok}"
            )

    lines.append(f"\n改动资产 {changed_assets} 个")
    report = "\n".join(lines)
    unreal.log(report)

    out = unreal.Paths.project_saved_dir() + "fix_call_durations.txt"
    with open(out, "w", encoding="utf-8") as f:
        f.write(report)
    unreal.log(f"[通话时长] 报告写到 {out}")


run()
