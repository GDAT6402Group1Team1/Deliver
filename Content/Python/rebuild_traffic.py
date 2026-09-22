# -*- coding: utf-8 -*-
"""按正确顺序把整套交通数据重建一遍。

存在的理由：路口盒子（BP_Intersection）是按 Inter_* 的 Box 位置算出来的，
**车道一重新生成，盒子就过期**——间隙一改、车道名单一改，所有 Inter_ Box 都挪了位，
旧盒子罩不住它们，红绿灯就不工作了。

这个坑已经踩过两次（两次都是我漏了让你跑 gen_intersections.py），
所以把顺序固化进脚本，而不是靠记性。

顺序（本脚本只跑前两步，后两步必须你自己另起一次 py 执行）：
    1. gen_traffic_lanes.py    直行车道 + 路口段        <- 这里跑
    2. gen_intersections.py    路口盒，依赖 1 的 Box 位置  <- 这里跑
    3. fill_turn_splines.py    左右转曲线，依赖 1 的样条端点
    4. fix_t_junctions.py      T 形路口修正，依赖 3 的转弯曲线
每一步各自会写自己的报告到 Saved/。

为什么 3、4 不能接在同一次执行里（实测三轮，别再试了）：
在同一次 py 执行里写这些样条，写进去的全是**马上要作废的组件**——
回读时组件名已经变成 TRASH_SplineComponent_xxxx、点数 0，存盘什么都没有。
单独跑一次 `py fill_turn_splines.py` 就完全正常（134/134 一致）。
试过的、被推翻的解释：
  - "旧 actor 没被 GC" —— 加了 is_valid 护栏，一条都没拦到，actor 全是有效的
  - "构造脚本要到下一帧才重跑" —— 挂 slate 回调等 5 帧再写，354/354 照样全灭
所以关键不是帧数、也不是 actor 生命周期，是必须另起一次执行。
真正的机制还没查清，但现象稳定可复现。

顺序更不能换：转弯没写成功就跑第 4 步的话，它会把蓝图默认的 100cm 残桩
当成右转抄进直行样条——实测毁过 44 条直行。现在第 4 步加了残桩护栏会拒绝
并报警，但那是兜底，不是许可证。

用法：py rebuild_traffic.py
"""

import os
import time
import traceback

import unreal

STEPS = [
    ("gen_traffic_lanes.py", "直行车道 + 路口段"),
    ("gen_intersections.py", "路口盒（依赖上一步的 Box 位置）"),
]
DEFERRED = [
    ("fill_turn_splines.py", "左右转曲线（等构造脚本跑完再写）"),
    ("fix_t_junctions.py", "T 形路口：直行合并到右转、删掉没人走的段"),
]
WAIT_FRAMES = 5    # 只剩报告里引用：当初试过等 5 帧，没用，已放弃自动接力
SKIP = []          # 想跳过某步就把文件名填进来

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = unreal.Paths.project_saved_dir() + "rebuild_traffic.txt"
KEY = "_delivery_rebuild_traffic"   # 回调句柄挂在 unreal 模块上才跨得过脚本结束
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[rebuild] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run_step(name, what):
    """跑一个子脚本，成功返回 True。"""
    path = os.path.join(HERE, name)
    if not os.path.exists(path):
        w("!! 找不到 %s，中止" % path)
        return False
    w("=" * 62)
    w("%s  —— %s" % (name, what))
    w("=" * 62)
    t0 = time.time()
    try:
        with open(path, encoding="utf-8") as fh:
            code = fh.read()
        # 每步独立的命名空间，避免上一步的全局变量串进下一步
        exec(compile(code, path, "exec"), {"__name__": "__main__",
                                           "__file__": path})
        w("  完成，用时 %.1f 秒" % (time.time() - t0))
        w("")
        return True
    except Exception:
        w("  !! 这一步抛异常，后面的步骤不再执行：")
        for ln in traceback.format_exc().split("\n")[-12:]:
            w("     %s" % ln)
        w("")
        return False


def run():
    w("重建交通数据：前 %d 步在这里跑，后 %d 步你自己另起一次 py 跑"
      % (len([s for s in STEPS if s[0] not in SKIP]), len(DEFERRED)))
    w("")
    for name, what in STEPS:
        if name in SKIP:
            w("跳过 %s" % name)
            continue
        if not run_step(name, what):
            flush()
            return
        flush()

    todo = [(nm, wt) for nm, wt in DEFERRED if nm not in SKIP]
    if not todo:
        w("关卡尚未保存。")
        flush()
        return

    w("=" * 62)
    w("前 %d 步完成。剩下这两步必须**你自己另起一次 py 执行**：" % len(STEPS))
    for i, (nm, wt) in enumerate(todo, 1):
        w("   %d. py %s   —— %s" % (i, nm, wt))
    w("")
    w("为什么不能自动接着跑（实测三轮，不要再试了）：")
    w("在同一次 py 执行里写这些样条，写进去的全是马上要作废的组件")
    w("（名字带 TRASH_ 前缀），脚本内部回读就对不上，存盘什么都没有。")
    w("试过挂 slate 每帧回调等 %d 帧再写，照样全灭——所以关键不是帧数，" % WAIT_FRAMES)
    w("是必须另起一次执行。原因还没查清，但现象稳定可复现。")
    w("")
    w("顺序不能换：转弯没写成功就跑 T 形修正的话，它会把蓝图默认的")
    w("100cm 残桩当成右转抄进直行样条（实测毁过 44 条）。现在那边加了")
    w("残桩护栏会拒绝并报警，但别依赖护栏——按顺序跑。")
    w("关卡尚未保存。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[rebuild] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常：")
    lines.append(err)
    flush()
