# -*- coding: utf-8 -*-
"""把某个路口"降级成普通路"：删掉 BP_Intersection 盒子，并解除它管辖的段的红灯。

背景（2026-10-29）
------------------
`BranchRoad_R020` 的交通线已被手动删除，于是它和 `MainRoad_R003` 的交叉点
（`Intersection_15` 附近）成了"只有一条路的假路口"——交叉方向没有车流，
在那儿立一盏灯只会无故拦车。

**只删盒子是不够的，而且很危险。** `Inter_` 段的 `StopAtInter` 是实例上
序列化的值（实测默认 `True`）。盒子一删，就没有任何 `BP_Intersection` 去
`GetOverlappingActors` 找到它们、把 `StopAtInter` 刷成 false，而车的
`TraceForIntersection` 照样能 cast 到这些段、读到 `True` → `StopCar` →
`PollForGreen` 永远等不到绿灯。**这正是最初 I08 卡车的那条机制。**
所以必须先把它们的 `StopAtInter` 写成 false，再删盒子。

**`Inter_` 段本身不能删**：它们是路口内部那一截**行驶路径**，
删了会在 `Lane_..._S07` 和 `_S08` 之间留下约 1900cm 没有样条的缺口
（`TARGET_GAP` 只有 150），车得靠 `TraceForNewPath` 盲接，缺口超过 250
就容易接不上——那是把一个 bug 换成另一个。保留它们、只是不再拦车。

分组判据用**盒子重叠**，和蓝图运行时 `GetOverlappingActors` 完全一致，
不用距离聚类（2500cm 聚类会把相邻路口并进来，误伤别的路口的段）。

永久性：下次重新生成车道时，这个路口会原样长回来——除非
`gen_traffic_lanes.py` 的 `NO_JUNCTION_WITH` 里有 `BranchRoad_R020`（已加）。
光加 `EXCLUDE_ROADS` 没用：被排除的样条仍然参与相交检测。

DRY_RUN=True 只报不改（默认）。
用法：py drop_intersection.py
"""

import traceback

import unreal

DRY_RUN = True
TARGET_INTERSECTIONS = ["Intersection_15"]
INTER_PREFIX = "BP_Intersection"
CHILD_PREFIX = "Inter_"

OUT = unreal.Paths.project_saved_dir() + "drop_intersection.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[drop] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def stop_prop(a):
    """StopAtInter 的真实属性名。蓝图变量用原名，snake_case 只适用于原生属性。"""
    for n in ("StopAtInter", "stopatinter", "stop_at_inter"):
        try:
            a.get_editor_property(n)
            return n
        except Exception:
            continue
    return None


def box_aabb(a):
    """Box 的世界 AABB。Box 跟着行驶方向转过，所以逐轴外接（保守偏大）。"""
    for c in a.get_components_by_class(unreal.BoxComponent):
        e = c.get_unscaled_box_extent()
        sc = a.get_actor_scale3d()
        hx, hy, hz = e.x * sc.x, e.y * sc.y, e.z * sc.z
        try:
            center, rot = c.get_world_location(), c.get_world_rotation()
        except Exception:
            center, rot = a.get_actor_location(), a.get_actor_rotation()
        f = unreal.MathLibrary.get_forward_vector(rot)
        r = unreal.MathLibrary.get_right_vector(rot)
        u = unreal.MathLibrary.get_up_vector(rot)
        return center, unreal.Vector(
            abs(f.x) * hx + abs(r.x) * hy + abs(u.x) * hz,
            abs(f.y) * hx + abs(r.y) * hy + abs(u.y) * hz,
            abs(f.z) * hx + abs(r.z) * hy + abs(u.z) * hz)
    return None


def overlaps(a_box, b_box):
    (ac, ah), (bc, bh) = a_box, b_box
    for axis in ("x", "y", "z"):
        if abs(getattr(ac, axis) - getattr(bc, axis)) > getattr(ah, axis) + getattr(bh, axis):
            return False
    return True


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 路口降级成普通路 ===")
    w(u"关卡：%s     模式：%s"
      % (world.get_name() if world else u"?", u"只报不改" if DRY_RUN else u"**写入**"))
    w(u"目标：%s" % u"、".join(TARGET_INTERSECTIONS))

    inters, segs, others = [], [], []
    for a in eas.get_all_level_actors():
        cls = a.get_class().get_name()
        lbl = a.get_actor_label()
        if cls.startswith(INTER_PREFIX) and "Child" not in cls:
            (inters if lbl in TARGET_INTERSECTIONS else others).append(a)
        elif lbl.startswith(CHILD_PREFIX):
            segs.append(a)
    w(u"找到目标路口 %d 个；其余路口 %d 个；Inter_ 段 %d 个"
      % (len(inters), len(others), len(segs)))
    if not inters:
        w(u"！一个目标都没找到。现有路口名（前 20）：")
        for a in others[:20]:
            w(u"    %s" % a.get_actor_label())
        return

    other_boxes = [(a.get_actor_label(), box_aabb(a)) for a in others]
    other_boxes = [(n, b) for n, b in other_boxes if b]

    total_cleared, to_delete = 0, []
    for inter in inters:
        bb = box_aabb(inter)
        loc = inter.get_actor_location()
        w(u"")
        w(u"── %s @ (%.0f, %.0f, %.0f)"
          % (inter.get_actor_label(), loc.x, loc.y, loc.z))
        if bb is None:
            w(u"    ！这个路口没有 Box，查不出它管哪些段。只删 actor。")
            to_delete.append(inter)
            continue

        mine = []
        for s in segs:
            sb = box_aabb(s)
            if sb and overlaps(bb, sb):
                # 被别的路口也罩住的段不能动：删掉本盒子之后另一个路口还在管它，
                # 把 StopAtInter 写成 false 只会被那个路口下一帧刷回去，
                # 而且会让那个路口的相位少一段。
                shared = [n for n, ob in other_boxes if overlaps(ob, sb)]
                mine.append((s, shared))
        w(u"    它的盒子罩住 %d 段：" % len(mine))
        for s, shared in mine:
            pn = stop_prop(s)
            cur = s.get_editor_property(pn) if pn else u"<读不到>"
            note = u""
            if shared:
                note = u"  ← 也被 %s 罩着，**不动它**" % u"、".join(shared)
            w(u"        %-40s StopAtInter=%s%s"
              % (s.get_actor_label()[:40], cur, note))

            if shared or pn is None:
                continue
            if cur is False:
                continue
            if not DRY_RUN:
                try:
                    s.modify(True)
                    s.set_editor_property(pn, False)
                    back = s.get_editor_property(pn)
                    if back is not False:
                        w(u"          ！写了但回读是 %s" % back)
                        continue
                except Exception as exc:
                    w(u"          ！写失败：%s" % str(exc)[:60])
                    continue
            total_cleared += 1
        to_delete.append(inter)

    w(u"")
    w(u"=" * 72)
    if DRY_RUN:
        w(u"只报不改。确认上面每一段都该解除之后，把 DRY_RUN 改 False 再跑。")
        w(u"届时会：把 %d 段的 StopAtInter 写成 false，再删掉 %d 个路口 actor。"
          % (total_cleared, len(to_delete)))
        return

    w(u"已把 %d 段的 StopAtInter 写成 false。" % total_cleared)
    for a in to_delete:
        nm = a.get_actor_label()
        try:
            eas.destroy_actor(a)
            w(u"已删除路口 actor：%s" % nm)
        except Exception as exc:
            w(u"！删 %s 失败：%s" % (nm, str(exc)[:60]))
    w(u"")
    w(u"**记得 Ctrl+S 保存关卡。**")
    w(u"复核：py audit_light_groups.py —— 那个位置应该不再出现在任何分组里。")
    w(u"注意顺序：先写 StopAtInter=false 再删盒子。反过来的话盒子没了、")
    w(u"段还是 true，就没人再去刷它——车会在那儿永远等绿灯（最初 I08 的机制）。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[drop] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[drop] 写入 %s" % OUT)
