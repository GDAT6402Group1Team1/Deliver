# -*- coding: utf-8 -*-
"""把路面加厚一点。

背景：路面是 Landscape 上 1982 个 SplineMeshComponent，网格 1M_CubeWithSocket(100^3)，
被压成 1080 宽 x 6 厚 —— 薄得像纸，侧面几乎看不见，也没有任何容错余量。

当前 20cm（从 6 起，两次对称加厚）。注意**填补空隙不是靠加厚**——那件事由绿色裙边
(gen_road_skirt.py) 负责；这里只是让板子本身厚实一点。

几何：Cube 中心对齐，缩放 s 时板子是 ±50s。0.06 -> ±3cm，0.20 -> ±10cm，
所以相对原始状态顶面抬高 7cm、底面下降 7cm。没有去动 StartOffset 把顶面按回原位
（那个参数的单位/作用时机没把握），几厘米的抬升对地形(-40)和裙边都无影响。

注意：这些组件是地形样条系统自动生成的。如果之后重新生成样条，
改动可能被冲掉——这点还没验证过。

用法：py thicken_road.py
"""

import traceback

import unreal

TARGET_THICKNESS = 26.0        # 目标厚度 cm。6 -> 14 -> 20，每次上下对称生长。
                               # 这次是为了让顶面再抬 3cm（20-14=6，对称分摊即上下各 3）。
                               # 没有用 StartOffset 做单侧生长：那个参数的单位/作用时机
                               # 没把握，而底面多沉 3cm 反正埋在裙边里看不见。
                               # 不是用加厚来填空隙——那件事交给绿色裙边了；
                               # 这里只是让路面本身别薄得像纸，顺便多一点容错。
MESH_HEIGHT = 100.0            # 1M_CubeWithSocket 原始高度
LIMIT = 0                      # >0 = 只改前 N 个做测试；0 = 全部

OUT = unreal.Paths.project_saved_dir() + "thicken_road.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[thicken] %s" % s)


def get_scales(c):
    try:
        p = c.get_editor_property("spline_params")
    except Exception:
        return None, None, None
    ss = es = None
    try:
        ss = p.get_editor_property("start_scale")
    except Exception:
        pass
    try:
        es = p.get_editor_property("end_scale")
    except Exception:
        pass
    return p, ss, es


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    land = None
    for a in eas.get_all_level_actors():
        if a.get_class().get_name() == "Landscape":
            land = a
            break
    if land is None:
        w("!! 找不到 Landscape")
        return

    sms = list(land.get_components_by_class(unreal.SplineMeshComponent))
    w("Landscape 上 SplineMeshComponent %d 个" % len(sms))
    if not sms:
        return

    new_y = TARGET_THICKNESS / MESH_HEIGHT
    w("目标厚度 %.0f cm  ->  Scale.Y = %.4f（原来约 0.06 = 6cm）"
      % (TARGET_THICKNESS, new_y))
    w("顶面预计抬高 %.1f cm，底面下降 %.1f cm（板子上下对称生长）"
      % (TARGET_THICKNESS / 2.0 - 3.0, TARGET_THICKNESS / 2.0 - 3.0))
    w("")

    # 先看一个样例，确认能读能写
    p0, ss0, es0 = get_scales(sms[0])
    if p0 is None or ss0 is None:
        w("!! 读不到 spline_params / start_scale，无法继续")
        return
    w("样例 %s: start_scale=(%.3f, %.3f)  end_scale=%s"
      % (sms[0].get_name(), ss0.x, ss0.y,
         ("(%.3f, %.3f)" % (es0.x, es0.y)) if es0 else "None"))
    w("")

    targets = sms[:LIMIT] if LIMIT > 0 else sms
    changed = failed = 0
    for c in targets:
        p, ss, es = get_scales(c)
        if p is None or ss is None:
            failed += 1
            continue
        try:
            p.set_editor_property("start_scale", unreal.Vector2D(ss.x, new_y))
            if es is not None:
                p.set_editor_property("end_scale", unreal.Vector2D(es.x, new_y))
            # 结构体属性取出来是副本，必须写回去
            c.set_editor_property("spline_params", p)
            for meth in ("update_mesh", "update_render_state_and_collision"):
                if hasattr(c, meth):
                    try:
                        getattr(c, meth)()
                        break
                    except Exception:
                        pass
            c.modify()
            changed += 1
        except Exception as exc:
            if failed < 3:
                w("   写入失败(%s): %s" % (c.get_name(), str(exc)[:80]))
            failed += 1

    w("已修改 %d 个，失败 %d 个%s"
      % (changed, failed, "（LIMIT=%d 测试模式）" % LIMIT if LIMIT > 0 else ""))

    # 回读验证
    p1, ss1, _e = get_scales(targets[0])
    if ss1 is not None:
        w("回读样例: start_scale=(%.3f, %.3f)  -> 厚度 %.0f cm"
          % (ss1.x, ss1.y, ss1.y * MESH_HEIGHT))
        if abs(ss1.y - new_y) > 1e-4:
            w("!! 回读值和目标不符，可能被系统重置了")

    w("")
    w("顶面只抬高几厘米，地形(-40)和裙边都还有充足余量，不必重跑 deform。")
    w("关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[thicken] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
