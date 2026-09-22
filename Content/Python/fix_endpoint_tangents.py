# -*- coding: utf-8 -*-
"""就地把所有交通样条的首尾点型从 CURVE_CLAMPED 改成 CURVE。

为什么要改：UE 的 "Clamped" 自动切线，定义行为就是把**端点**和局部极值处的
切线归零。而 GetRotationAtDistanceAlongSpline 内部是 MakeFromXZ(切线, Up)，
切线为零时这个构造退化，前向掉回世界 +X。

实测：端点切线长恒为 0、旋转前向恒为 0.0°，而真实走向是 -96°。
车的探测球因此在"未来点被钳到样条末端"的那一两帧甩向世界 +X
（相对车头正好是右侧）。这也是 CLAUDE.md 里那条
"get_direction_at_distance_along_spline 返回零向量"的真正根因。

为什么不重跑生成器：生成器里已经改好了，但重跑会重建全部 actor，
把转弯曲线、T 形路口修正这些后续工作一起冲掉。点型是纯粹的就地修改，
**控制点一个都不动**——车道偏移、路口切断位置、高度、Box 尺寸全不变，
只有首尾各一段的插值形状会从"零切线的畸形贴合"变成正常曲线。

改完会回读验证：端点切线长不再为 0，才算真的生效。

用法：py fix_endpoint_tangents.py
"""

import math
import traceback

import unreal

LANE_TAG_PREFIX = "ClaudeGenLane"
DRY_RUN = False
BAD_ANGLE = 30.0         # 回读时，端点朝向和几何走向夹角超过这个值就算没修好
DELTA = 50.0

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "endpoint_tangents.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def mark_edited(sp):
    for n in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(n, True)
            return True
        except Exception:
            continue
    return False


def end_angle(sp, at_end):
    """端点处 旋转前向 与 几何走向 的夹角；取不到返回 None。"""
    L = sp.get_spline_length()
    if L < 1.0:
        return None
    d = L if at_end else 0.0
    r = sp.get_rotation_at_distance_along_spline(d, WS)
    f = r.get_forward_vector()
    fh = (f.x * f.x + f.y * f.y) ** 0.5
    if fh < 1e-4:
        return 999.0
    a0 = sp.get_location_at_distance_along_spline(max(0.0, d - DELTA), WS)
    a1 = sp.get_location_at_distance_along_spline(min(L, d + DELTA), WS)
    gx, gy = a1.x - a0.x, a1.y - a0.y
    gh = (gx * gx + gy * gy) ** 0.5
    if gh < 1e-4:
        return None
    dot = max(-1.0, min(1.0, (f.x / fh) * (gx / gh) + (f.y / fh) * (gy / gh)))
    return math.degrees(math.acos(dot))


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    comps = []
    for a in eas.get_all_level_actors():
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        for c in a.get_components_by_class(unreal.SplineComponent):
            if c.get_number_of_spline_points() >= 2:
                comps.append((a, c))

    w("交通样条组件 %d 个" % len(comps))
    if not comps:
        w("!! 一个都没有。")
        flush()
        return

    before = [end_angle(c, True) for _a, c in comps[:200]]
    bad_before = sum(1 for v in before if v is not None and v > BAD_ANGLE)
    w("改之前：抽查 %d 个末端，朝向偏离超过 %.0f° 的有 %d 个"
      % (len(before), BAD_ANGLE, bad_before))
    w("")
    flush()

    if DRY_RUN:
        w("DRY_RUN=True，什么都没改。")
        flush()
        return

    done = fail = 0
    for a, sp in comps:
        n = sp.get_number_of_spline_points()
        # mark_edited 要在改动**之前**调：它是 set_editor_property，
        # 在组件上改属性会触发构造脚本重跑、组件整批重建，
        # 放在最后的话改动会落在被丢弃的那批组件上。
        marked = mark_edited(sp)
        try:
            # modify() 同样必须在改动之前，否则只改内存、存盘不序列化
            sp.modify(True)
            a.modify(True)
        except Exception:
            pass
        try:
            for i in (0, n - 1):
                sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
            sp.update_spline()
            if marked:
                done += 1
            else:
                fail += 1
        except Exception as exc:
            fail += 1
            if fail <= 5:
                w("!! %s.%s: %s"
                  % (a.get_actor_label()[:26], sp.get_name(), str(exc)[:60]))

    w("已改 %d 个，失败 %d 个" % (done, fail))
    w("")

    # --- 回读验证 ---
    after = [end_angle(c, True) for _a, c in comps[:200]]
    bad_after = sum(1 for v in after if v is not None and v > BAD_ANGLE)
    w("=" * 80)
    w("回读验证")
    w("=" * 80)
    w("末端朝向偏离超过 %.0f° 的：改前 %d 个 -> 改后 %d 个"
      % (BAD_ANGLE, bad_before, bad_after))

    zero = 0
    for _a, sp in comps[:200]:
        L = sp.get_spline_length()
        t = sp.get_tangent_at_distance_along_spline(L, WS)
        if (t.x * t.x + t.y * t.y + t.z * t.z) ** 0.5 < 1e-3:
            zero += 1
    w("末端切线仍为零的：%d 个（改之前是全部）" % zero)
    w("")
    if bad_after == 0 and zero == 0:
        w("修好了。车的探测球不会再在段末端那一两帧甩向世界 +X。")
    else:
        w("!! 还有没修好的。可能是这几条样条的最后两个控制点重合"
          "（相邻点同坐标时自动切线也是零），要回生成器查。")
    w("")
    w("注意：控制点一个都没动，车道偏移/路口位置/高度/Box 全不变，")
    w("      只有首尾各一段的插值形状变了。生成器里也已经同步改好，")
    w("      以后重新生成不用再跑本脚本。")
    w("关卡尚未保存。")
    flush()
    unreal.log("[endtan] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[endtan] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
