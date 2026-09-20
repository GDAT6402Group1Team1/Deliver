# -*- coding: utf-8 -*-
"""读出路面方块的真实宽度和厚度。

已确认：路面 = Landscape 上 1982 个 SplineMeshComponent，
网格是 1M_CubeWithSocket（原始 100×100×100）。

上一版没读到缩放，因为 start_scale/end_scale 不是组件的直接属性，
它们包在 SplineParams（FSplineMeshParams 结构体）里。

forward_axis = X，所以横截面是 YZ：
    成品宽度 = 100 × Scale.X
    成品厚度 = 100 × Scale.Y

用法：py check_road_mesh.py
"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "road_mesh.txt"
MESH_SIZE = 100.0        # 1M_CubeWithSocket 原始边长
lines = []


def w(s=""):
    lines.append(str(s))


def read_params(c):
    """返回 (start_scale, end_scale)，取不到返回 (None, None)。"""
    for pn in ("spline_params", "SplineParams"):
        try:
            p = c.get_editor_property(pn)
        except Exception:
            continue
        ss = es = None
        for n in ("start_scale", "StartScale"):
            try:
                ss = p.get_editor_property(n)
                break
            except Exception:
                pass
        for n in ("end_scale", "EndScale"):
            try:
                es = p.get_editor_property(n)
                break
            except Exception:
                pass
        if ss is not None or es is not None:
            return ss, es
    # 退而求其次：组件自己的取值方法
    try:
        return c.get_start_scale(), c.get_end_scale()
    except Exception:
        return None, None


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    lands = [a for a in eas.get_all_level_actors()
             if "Landscape" in a.get_class().get_name()]

    for a in lands:
        sms = list(a.get_components_by_class(unreal.SplineMeshComponent))
        w("=" * 62)
        w("%s   SplineMeshComponent %d 个" % (a.get_actor_label(), len(sms)))
        w("=" * 62)
        if not sms:
            continue

        # 先把结构体的字段名摸清楚（只打一次）
        try:
            p = sms[0].get_editor_property("spline_params")
            w("SplineParams 类型 = %s" % type(p).__name__)
            try:
                w("  to_tuple 字段数 = %s" % len(p.to_tuple()))
            except Exception:
                pass
            w("  可读字段: %s" % ", ".join(
                sorted(n for n in dir(p) if not n.startswith("_"))[:24]))
        except Exception as exc:
            w("读 spline_params 失败: %s" % str(exc)[:80])
        w("")

        widths, thicks = [], []
        bad = 0
        for c in sms:
            ss, es = read_params(c)
            if ss is None and es is None:
                bad += 1
                continue
            for s in (ss, es):
                if s is None:
                    continue
                try:
                    widths.append(MESH_SIZE * s.x)
                    thicks.append(MESH_SIZE * s.y)
                except Exception:
                    bad += 1

        if not widths:
            w("!! 一个缩放都没读到（失败 %d 个），下面给几个样例的原始 dump:" % bad)
            for c in sms[:3]:
                try:
                    w("   %s -> %s" % (c.get_name(),
                                       repr(c.get_editor_property("spline_params"))[:200]))
                except Exception as exc:
                    w("   %s ! %s" % (c.get_name(), str(exc)[:60]))
        else:
            widths.sort()
            thicks.sort()
            n = len(widths)
            w("读到 %d 个缩放值（失败 %d）" % (n, bad))
            w("")
            w("成品路面宽度 (= 100 × Scale.X):")
            w("   最小 %.0f   中位 %.0f   最大 %.0f cm" % (widths[0], widths[n // 2], widths[-1]))
            w("成品路面厚度 (= 100 × Scale.Y):")
            w("   最小 %.0f   中位 %.0f   最大 %.0f cm" % (thicks[0], thicks[n // 2], thicks[-1]))
            w("")
            w("对照: 之前用 Cube 建筑和样条点 Y 缩放量出的路宽是 1000 cm")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[road_mesh] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[road_mesh] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
