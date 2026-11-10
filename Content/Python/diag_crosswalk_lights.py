# -*- coding: utf-8 -*-
u"""只读：为「只在斑马线处摆人行红绿灯」查清现状。

一次回答四个问题：
  ① 图里有几条斑马线、各自的锚点/行驶方向/横向跨度（= 路宽）
  ② 每条斑马线压在哪条 `Inter_` 段上（判据抄 sync_crosswalk_tags.py，
     段入口后方 150~800、横向 ≤1100），那条段的 `LightNumber` 是几
  ③ 同一个路口里两组是否都齐 —— 「行人灯和车灯相反」要成立，
     前提是每条斑马线能唯一确定一个组
  ④ 图里现在还剩几盏灯（应该是 0，11/5 删过 133 盏）

**只读**：不 spawn、不删、不改 tag、不存关卡。
无界面：
    UnrealEditor-Cmd <工程>.uproject -run=pythonscript \
        -script=<本文件绝对路径> -unattended -nopause -nosplash -NullRHI
"""
import json
import math
import re

import unreal

LEVEL = u"/Game/Level/TestForCharacter"
STOP_TAG = u"Crosswalk"
SURFACE_TAG = u"DeliveryCrosswalkSurfaceV2"
INTER = re.compile(r'^Inter_(.+)_([+-]\d+)_I\d+$')
BEHIND_MIN, BEHIND_MAX, LATERAL_MAX = 150.0, 800.0, 1100.0
GREEN_FIRST_GROUP = 0          # 和 gen_traffic_lights.py / tune_traffic_lights.py 一致
WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + u"diag_crosswalk_lights.json"

_lines = []


def w(s=u""):
    _lines.append(s)
    unreal.log(u"[xwalklight] %s" % s)


def entry_and_dir(actor):
    u"""段入口点和行驶方向；方向用两个采样点作差（端点切线会退化成零向量）"""
    main = None
    splines = actor.get_components_by_class(unreal.SplineComponent)
    for sp in splines:
        if sp.get_name() == u"Spline":
            main = sp
            break
    if main is None and splines:
        main = splines[0]
    if main is None or main.get_number_of_spline_points() < 2:
        return None
    length = main.get_spline_length()
    if length <= 1.0:
        return None
    p0 = main.get_location_at_distance_along_spline(0.0, WS)
    p1 = main.get_location_at_distance_along_spline(min(200.0, length), WS)
    dx, dy = p1.x - p0.x, p1.y - p0.y
    n = math.hypot(dx, dy)
    if n < 0.01:
        return None
    return p0, (dx / n, dy / n)


def run():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(LEVEL):
        w(u"！加载不了 %s" % LEVEL)
        return
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = [a for a in eas.get_all_level_actors() if a]

    marks = [a for a in actors
             if a.get_actor_label().startswith(u"Crosswalk_")
             or SURFACE_TAG in [str(t) for t in a.tags]]
    lights = [a for a in actors
              if a.get_class().get_name().lower().startswith(u"trafficlight")
              or u"ClaudeGenTrafficLight" in [str(t) for t in a.tags]]

    segs = []
    for a in actors:
        if not INTER.match(a.get_actor_label()):
            continue
        ed = entry_and_dir(a)
        if ed is None:
            continue
        tagged = STOP_TAG in [str(t) for t in a.tags]
        ln = None
        try:
            ln = a.get_editor_property(u"LightNumber")
        except Exception:
            try:
                ln = a.get_editor_property(u"Light Number")
            except Exception:
                ln = None
        segs.append(dict(actor=a, label=a.get_actor_label(), entry=ed[0],
                         fwd=ed[1], tagged=tagged, light=ln))

    w(u"=== 斑马线与人行灯现状 ===")
    w(u"斑马线 actor %d、Inter_ 段 %d（其中带 Crosswalk tag %d）、现存灯 %d 盏"
      % (len(marks), len(segs), sum(1 for s in segs if s['tagged']), len(lights)))

    # 每条段找它后方那条斑马线，再反过来整理成「斑马线 → 段」
    per_mark = {}
    for s in segs:
        ex, ey = s['entry'].x, s['entry'].y
        fx, fy = s['fwd']
        best, best_behind = None, None
        for m in marks:
            loc = m.get_actor_location()
            dx, dy = loc.x - ex, loc.y - ey
            along = dx * fx + dy * fy
            if along >= 0.0:
                continue
            behind = -along
            lateral = abs(-dx * fy + dy * fx)
            if BEHIND_MIN <= behind <= BEHIND_MAX and lateral <= LATERAL_MAX:
                if best is None or behind < best_behind:
                    best, best_behind = m, behind
        if best is not None:
            per_mark.setdefault(best.get_actor_label(), []).append(
                dict(seg=s['label'], light=s['light'], tagged=s['tagged'],
                     behind=round(best_behind, 1)))

    report = []
    w(u"")
    w(u"--- 逐条斑马线 ---")
    for m in sorted(marks, key=lambda a: a.get_actor_label()):
        label = m.get_actor_label()
        loc = m.get_actor_location()
        fwd = m.get_actor_forward_vector()
        right = m.get_actor_right_vector()
        origin, extent = m.get_actor_bounds(False)
        hits = per_mark.get(label, [])
        groups = sorted({h['light'] for h in hits if h['light'] is not None})
        entry = dict(
            crosswalk=label,
            location=[loc.x, loc.y, loc.z],
            forward=[fwd.x, fwd.y, fwd.z],
            right=[right.x, right.y, right.z],
            bounds_center=[origin.x, origin.y, origin.z],
            bounds_extent=[extent.x, extent.y, extent.z],
            segments=hits,
            light_groups=groups,
            vehicle_starts_green=[(g % 2) == (GREEN_FIRST_GROUP % 2) for g in groups],
        )
        report.append(entry)
        w(u"%-34s 组=%s 段=%s" % (label, groups or u"（没匹配上）",
                                 u"/".join(h['seg'] for h in hits) or u"-"))
        if not hits:
            w(u"    ！没有段压在它后方 —— 摆灯时无法确定该和哪一组相反")
        elif len(groups) > 1:
            w(u"    ！压着两个不同的灯组 %s —— 相反相对于谁要人定" % groups)
        untagged = [h['seg'] for h in hits if not h['tagged']]
        if untagged:
            w(u"    ！这些段没有 Crosswalk 停车 tag：%s" % u", ".join(untagged))

    # 路口层面：两组齐不齐
    w(u"")
    w(u"--- 带 tag 的段按灯组 ---")
    by_light = {}
    for s in segs:
        if s['tagged']:
            by_light.setdefault(s['light'], []).append(s['label'])
    for k in sorted(by_light, key=lambda x: (x is None, x)):
        w(u"  组 %s：%d 条" % (k, len(by_light[k])))

    w(u"")
    w(u"--- 现存灯 ---")
    for a in lights:
        w(u"  %s (%s)" % (a.get_actor_label(), a.get_class().get_name()))
    if not lights:
        w(u"  一盏都没有（符合 11/5 删掉 133 盏之后的状态）")

    with open(OUT, u"w") as f:
        json.dump(dict(level=LEVEL, crosswalks=len(marks), segments=len(segs),
                       tagged=sum(1 for s in segs if s['tagged']),
                       existing_lights=[a.get_actor_label() for a in lights],
                       green_first_group=GREEN_FIRST_GROUP,
                       detail=report), f, ensure_ascii=False, indent=1)
    w(u"")
    w(u"明细写到 %s" % OUT)


run()
