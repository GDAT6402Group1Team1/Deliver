# -*- coding: utf-8 -*-
"""按正确顺序把整套交通数据重建一遍。

存在的理由：路口盒子（BP_Intersection）是按 Inter_* 的 Box 位置算出来的，
**车道一重新生成，盒子就过期**——间隙一改、车道名单一改，所有 Inter_ Box 都挪了位，
旧盒子罩不住它们，红绿灯就不工作了。

这个坑已经踩过两次（两次都是我漏了让你跑 gen_intersections.py），
所以把顺序固化进脚本，而不是靠记性。

顺序：
    1. gen_traffic_lanes.py    直行车道 + 路口段
    2. gen_intersections.py    路口盒，依赖 1 的 Box 位置
    3. fill_turn_splines.py    左右转曲线，依赖 1 的样条端点
每一步各自会写自己的报告到 Saved/。

用法：py rebuild_traffic.py
"""

import os
import time
import traceback

import unreal

STEPS = [
    ("gen_traffic_lanes.py", "直行车道 + 路口段"),
    ("gen_intersections.py", "路口盒（依赖上一步的 Box 位置）"),
    ("fill_turn_splines.py", "左右转曲线（依赖上一步的样条端点）"),
]
SKIP = []          # 想跳过某步就把文件名填进来

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = unreal.Paths.project_saved_dir() + "rebuild_traffic.txt"
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


def run():
    w("重建交通数据，共 %d 步" % len([s for s in STEPS if s[0] not in SKIP]))
    w("")
    for name, what in STEPS:
        if name in SKIP:
            w("跳过 %s" % name)
            continue
        path = os.path.join(HERE, name)
        if not os.path.exists(path):
            w("!! 找不到 %s，中止" % path)
            flush()
            return
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
        except Exception:
            w("  !! 这一步抛异常，后面的步骤不再执行：")
            for ln in traceback.format_exc().split("\n")[-12:]:
                w("     %s" % ln)
            flush()
            return
        w("")
        flush()

    w("=" * 62)
    w("全部完成。各步的详细报告见 Saved/ 下对应的 txt。")
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
