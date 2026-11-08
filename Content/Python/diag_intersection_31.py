# -*- coding: utf-8 -*-
u"""查 Intersection_31 的交通样条为什么接不上。只读。

判据和生成链保持一致（不另发明一套）：
  · 归属：路口自己的 Box 和段的入口点做 OBB 包含（= BP_Intersection 找子节点的判据）
  · 进得来：有 Lane_ 的**终点**落在本段起点 LINK_DIST 内，且**同向**
  · 出得去：有 Lane_ 的**起点**落在本段终点 LINK_DIST 内，且**同向**
    —— 同向判据必不可少：对向车道的起点就在附近（实测 360 cm），
       只看位置会把逆行当成合法下家（fix_t_junctions.py 栽过）
  · 端点切线：CURVE_CLAMPED 会把端点切线归零，车的路口探测会甩向世界 +X
  · 转弯样条：SplineLeft/SplineRight 是转弯曲线 / 直行副本 / 蓝图默认残桩

结果写 Saved/diag_intersection_31.txt
"""

import math
import re
import traceback

import unreal

LEVEL = u"/Game/Level/TestForCharacter"
TARGET = u"Intersection_31"
INTER_RE = re.compile(r'^Inter_(.+)_([+-]\d+)_I(\d+)$')
LANE_RE = re.compile(r'^Lane_(.+)_([+-]\d+)_L?(\d+)?')
LINK_DIST = 600.0
SAME_DIR_DOT = 0.5
STUB_LEN = 300.0
WS = unreal.SplineCoordinateSpace.WORLD

_lines = []


def w(s=u""):
    _lines.append(s)
    unreal.log(u"[i31] %s" % s)


def main_spline(actor):
    sps = actor.get_components_by_class(unreal.SplineComponent)
    for sp in sps:
        if sp.get_name() == u"Spline":
            return sp
    return sps[0] if sps else None


def named_spline(actor, name):
    for sp in actor.get_components_by_class(unreal.SplineComponent):
        if sp.get_name() == name:
            return sp
    return None


def ends(sp):
    u"""首尾控制点、首尾方向（两个采样点作差）、首尾切线长。"""
    n = sp.get_number_of_spline_points()
    if n < 2:
        return None
    length = sp.get_spline_length()
    p0 = sp.get_location_at_distance_along_spline(0.0, WS)
    pl = sp.get_location_at_distance_along_spline(length, WS)
    s = min(200.0, length * 0.5)
    d_start = sp.get_location_at_distance_along_spline(s, WS) - p0
    d_end = pl - sp.get_location_at_distance_along_spline(length - s, WS)

    def norm2(v):
        m = math.hypot(v.x, v.y)
        return (v.x / m, v.y / m) if m > 0.01 else None

    t0 = sp.get_tangent_at_distance_along_spline(0.0, WS)
    t1 = sp.get_tangent_at_distance_along_spline(length, WS)
    return dict(n=n, length=length, p0=p0, pl=pl,
                d0=norm2(d_start), d1=norm2(d_end),
                tan0=math.sqrt(t0.x * t0.x + t0.y * t0.y + t0.z * t0.z),
                tan1=math.sqrt(t1.x * t1.x + t1.y * t1.y + t1.z * t1.z))


def dist2(a, b):
    return math.hypot(a.x - b.x, a.y - b.y)


def dot2(u_, v_):
    return u_[0] * v_[0] + u_[1] * v_[1] if (u_ and v_) else -2.0


def box_of(actor):
    boxes = actor.get_components_by_class(unreal.BoxComponent)
    if not boxes:
        return None
    b = boxes[0]
    return (b.get_world_location(), b.get_scaled_box_extent(), b.get_world_rotation().yaw)


def inside(point, box):
    c, e, yaw = box
    dx, dy = point.x - c.x, point.y - c.y
    cs, sn = math.cos(math.radians(-yaw)), math.sin(math.radians(-yaw))
    return abs(dx * cs - dy * sn) <= e.x and abs(dx * sn + dy * cs) <= e.y


def read_prop(obj, name):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return None


def run():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(LEVEL):
        w(u"加载不了 %s" % LEVEL)
        return
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = [a for a in eas.get_all_level_actors() if a]

    target = None
    inters = []
    for a in actors:
        if a.get_class().get_name().startswith(u"BP_Intersection"):
            inters.append(a)
            if a.get_actor_label() == TARGET:
                target = a
    if target is None:
        w(u"找不到 %s" % TARGET)
        return

    tbox = box_of(target)
    tloc = target.get_actor_location()
    w(u"=== %s ===" % TARGET)
    w(u"actor 位置 (%.0f, %.0f, %.0f)" % (tloc.x, tloc.y, tloc.z))
    if tbox:
        w(u"路口盒 中心(%.0f, %.0f) 半尺寸(%.0f, %.0f) yaw %.1f"
          % (tbox[0].x, tbox[0].y, tbox[1].x, tbox[1].y, tbox[2]))
    else:
        w(u"！路口没有 Box 组件——它找不到任何子节点，这一个就足以让整个路口是死的")
    w(u"LightDuration = %s" % read_prop(target, u"LightDuration"))

    # 附近还有没有第二个路口（两套定时器抢同一批子节点）
    for other in inters:
        if other is target:
            continue
        d = dist2(other.get_actor_location(), tloc)
        if d < 2500.0:
            w(u"！附近 %.0f cm 还有一个路口 %s：两套定时器会抢着写同一批子节点"
              % (d, other.get_actor_label()))

    segs, lanes = [], []
    for a in actors:
        label = a.get_actor_label()
        m = INTER_RE.match(label)
        if m:
            sp = main_spline(a)
            e = ends(sp) if sp else None
            segs.append(dict(actor=a, label=label, road=m.group(1),
                             off=int(m.group(2)), idx=m.group(3), sp=sp, e=e))
            continue
        if label.startswith(u"Lane_"):
            sp = main_spline(a)
            e = ends(sp) if sp else None
            if e:
                lanes.append(dict(actor=a, label=label, e=e))

    owned = []
    if tbox:
        for s in segs:
            if s['e'] and inside(s['e']['p0'], tbox):
                owned.append(s)
    w(u"")
    w(u"归它管的路口段：%d 条（全图路口段 %d、车道段 %d）"
      % (len(owned), len(segs), len(lanes)))
    if not owned:
        w(u"！一条都没罩住——`BP_Intersection` 的 GetOverlappingActors 找不到子节点，")
        w(u"  那些 Inter_ 段的 StopAtInter 没人翻，车会在路口前等到关卡结束。")
        w(u"  这通常是车道重新生成之后忘了跑 gen_intersections.py。")

    for s in sorted(owned, key=lambda x: (x['road'], x['off'])):
        e = s['e']
        w(u"")
        w(u"— %s" % s['label'])
        if not e:
            w(u"  ！主样条点数 <2，取不到首尾")
            continue
        w(u"  点数 %d、长度 %.0f、起点(%.0f, %.0f, %.0f) 终点(%.0f, %.0f, %.0f)"
          % (e['n'], e['length'], e['p0'].x, e['p0'].y, e['p0'].z,
             e['pl'].x, e['pl'].y, e['pl'].z))
        w(u"  LightNumber=%s  StopAtInter=%s"
          % (read_prop(s['actor'], u"LightNumber"), read_prop(s['actor'], u"StopAtInter")))
        w(u"  端点切线长 起 %.1f / 终 %.1f%s"
          % (e['tan0'], e['tan1'],
             u"   ← 为零会让车的路口探测甩向世界 +X" if min(e['tan0'], e['tan1']) < 1.0 else u""))

        # 进得来？
        best_in = []
        for L in lanes:
            d = dist2(L['e']['pl'], e['p0'])
            if d <= LINK_DIST:
                best_in.append((d, dot2(L['e']['d1'], e['d0']), L['label']))
        best_in.sort()
        ok_in = [r for r in best_in if r[1] >= SAME_DIR_DOT]
        w(u"  进得来：%s" % (u"是" if ok_in else u"**否**"))
        for d, dt, lab in best_in[:4]:
            w(u"      上游候选 %-36s 距 %.0f cm、同向点积 %.2f%s"
              % (lab, d, dt, u"" if dt >= SAME_DIR_DOT else u"  ← 方向不对（逆行）"))
        if not best_in:
            w(u"      %.0f cm 内没有任何车道段的终点" % LINK_DIST)

        # 出得去？
        best_out = []
        for L in lanes:
            d = dist2(L['e']['p0'], e['pl'])
            if d <= LINK_DIST:
                best_out.append((d, dot2(e['d1'], L['e']['d0']), L['label']))
        best_out.sort()
        ok_out = [r for r in best_out if r[1] >= SAME_DIR_DOT]
        w(u"  出得去：%s" % (u"是" if ok_out else u"**否**"))
        for d, dt, lab in best_out[:4]:
            w(u"      下游候选 %-36s 距 %.0f cm、同向点积 %.2f%s"
              % (lab, d, dt, u"" if dt >= SAME_DIR_DOT else u"  ← 方向不对（逆行）"))
        if not best_out:
            w(u"      %.0f cm 内没有任何车道段的起点" % LINK_DIST)

        # 转弯样条
        for side in (u"SplineLeft", u"SplineRight"):
            sp = named_spline(s['actor'], side)
            if not sp:
                w(u"  %s：没有这个组件" % side)
                continue
            te = ends(sp)
            if not te:
                w(u"  %s：点数 %d（<2，退化）" % (side, sp.get_number_of_spline_points()))
                continue
            same = dist2(te['pl'], e['pl']) < 50.0 and te['n'] == e['n']
            stub = te['n'] <= 3 and te['length'] < STUB_LEN
            kind = (u"蓝图默认残桩" if stub else
                    (u"直行副本" if same else u"转弯曲线"))
            # 转弯终点有没有同向下家
            outs = [(dist2(L['e']['p0'], te['pl']), dot2(te['d1'], L['e']['d0']), L['label'])
                    for L in lanes if dist2(L['e']['p0'], te['pl']) <= LINK_DIST]
            outs = [o for o in outs if o[1] >= SAME_DIR_DOT]
            w(u"  %s：%s，点数 %d、长 %.0f、终点下家 %s"
              % (side, kind, te['n'], te['length'],
                 (u"有（%s）" % sorted(outs)[0][2]) if outs else u"**没有**"))

    out = unreal.Paths.project_saved_dir() + u"diag_intersection_31.txt"
    with open(out, u"w", encoding=u"utf-8") as fh:
        fh.write(u"\n".join(_lines))
    unreal.log(u"[i31] 报告写到 %s" % out)


try:
    run()
except Exception:
    unreal.log_error(traceback.format_exc())
