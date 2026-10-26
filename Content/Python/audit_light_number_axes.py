# -*- coding: utf-8 -*-
"""审计每个路口的 LightNumber 分组对不对。只读。

正确的判据（用户给的）：**同轴的同组，相交的不同组**。
"同轴"包含两种：同向（dot≈+1）和对向（dot≈-1）——对向车道是同一条路的两个方向，
必须一起放行，所以按**无向轴**归类，不看正负。

现在生成器用的是纯轴向优势（gen_traffic_lanes.py `set_light_number`）：
    |Δy| <= |Δx| → 保持 0（算沿 X）；否则设 1（算沿 Y）
这在正南正北的路网上没问题，但本图是自由角度的：
  · 45° 的斜路 |Δy| ≈ |Δx|，落哪边基本随机；
  · 两条都偏斜的路（例如 30° 和 120°，彼此垂直）可能被判到**同一边** →
    同组 → 这个路口永远不交替，而 Length==1 时还会恒红。
所以要按**实际走向**复核，而不是按世界坐标轴。

判据：
  · 轴角 = 段首末连线的方向角，归一化到 [0,180)（无向）
  · 两段轴角差 < SAME_AXIS_TOL(30°) → 同轴，**应当同组**
  · 轴角差 > PERP_MIN(40°)          → 相交，**应当异组**
  · 两者之间（30~40°）视为不确定，不判对错，只列出来

只读，不改任何值。结果写到 Saved/audit_light_number_axes.txt
用法：py audit_light_number_axes.py
"""

import math
import re
import traceback

import unreal

CLUSTER_DIST = 2500.0
CHILD_PREFIX = "Inter_"
SAME_AXIS_TOL = 30.0
PERP_MIN = 40.0
LABEL_RE = re.compile(r'^Inter_(.+)_([+-]\d+)_I(\d+)$')

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "audit_light_number_axes.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[axes] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def prop(a, name):
    for n in (name, name.lower()):
        try:
            return a.get_editor_property(n)
        except Exception:
            continue
    return None


def main_spline(a):
    best = None
    for c in a.get_components_by_class(unreal.SplineComponent):
        nm = c.get_name()
        if "Left" in nm or "Right" in nm:
            continue
        if best is None or nm in ("Spline", "SplineComponent"):
            best = c
    return best


def axis_angle(sp):
    """段首末连线的**无向**轴角，度，范围 [0,180)。"""
    n = sp.get_number_of_spline_points()
    if n < 2:
        return None
    p = sp.get_location_at_spline_point(0, WS)
    q = sp.get_location_at_spline_point(n - 1, WS)
    dx, dy = q.x - p.x, q.y - p.y
    if abs(dx) < 1e-6 and abs(dy) < 1e-6:
        return None
    a = math.degrees(math.atan2(dy, dx)) % 180.0
    return a


def axis_diff(a, b):
    """两个无向轴角的夹角，0~90。"""
    d = abs(a - b) % 180.0
    return min(d, 180.0 - d)


def road_of(label):
    m = LABEL_RE.match(label)
    return m.group(1) if m else u"?"


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== LightNumber 分组审计（按实际走向，只读）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))
    w(u"判据：轴角差 < %.0f° 应当同组；> %.0f° 应当异组" % (SAME_AXIS_TOL, PERP_MIN))

    segs = []
    for a in eas.get_all_level_actors():
        lbl = a.get_actor_label()
        if not lbl.startswith(CHILD_PREFIX):
            continue
        sp = main_spline(a)
        if sp is None:
            continue
        ang = axis_angle(sp)
        if ang is None:
            continue
        segs.append({"a": a, "label": lbl, "ang": ang,
                     "ln": prop(a, "LightNumber"), "road": road_of(lbl),
                     "loc": a.get_actor_location()})
    w(u"可分析的 Inter_ 段 %d 个" % len(segs))
    if not segs:
        w(u"！一个都没有。")
        return

    clusters = []
    for s in segs:
        got = None
        for c in clusters:
            if (c["center"] - s["loc"]).length() < CLUSTER_DIST:
                got = c
                break
        if got is None:
            clusters.append({"center": s["loc"], "segs": [s]})
        else:
            got["segs"].append(s)
            k = len(got["segs"])
            got["center"] = unreal.Vector(
                sum(x["loc"].x for x in got["segs"]) / k,
                sum(x["loc"].y for x in got["segs"]) / k,
                sum(x["loc"].z for x in got["segs"]) / k)
    w(u"聚类出 %d 个物理路口" % len(clusters))

    bad_same, bad_diff, multi, ok_n = [], [], [], 0
    for c in clusters:
        ss = c["segs"]
        probs = []
        for i in range(len(ss)):
            for j in range(i + 1, len(ss)):
                d = axis_diff(ss[i]["ang"], ss[j]["ang"])
                same_group = (str(ss[i]["ln"]) == str(ss[j]["ln"]))
                if d < SAME_AXIS_TOL and not same_group:
                    probs.append((u"同轴却异组", ss[i], ss[j], d))
                elif d > PERP_MIN and same_group:
                    probs.append((u"相交却同组", ss[i], ss[j], d))
        # 轴聚类：看这个路口一共有几个方向
        axes = []
        for s in ss:
            hit = None
            for ax in axes:
                if axis_diff(ax["ang"], s["ang"]) < SAME_AXIS_TOL:
                    hit = ax
                    break
            if hit is None:
                axes.append({"ang": s["ang"], "segs": [s]})
            else:
                hit["segs"].append(s)
        c["axes"] = axes
        c["probs"] = probs
        if len(axes) > 2:
            multi.append(c)
        if not probs:
            ok_n += 1
        else:
            if any(p[0] == u"同轴却异组" for p in probs):
                bad_same.append(c)
            if any(p[0] == u"相交却同组" for p in probs):
                bad_diff.append(c)

    w(u"")
    w(u"=" * 78)
    w(u"分组正确：%d 个    有问题：%d 个（相交却同组 %d，同轴却异组 %d）"
      % (ok_n, len(clusters) - ok_n, len(bad_diff), len(bad_same)))
    w(u"方向多于两个的路口：%d 个（两色分不开，要单独看）" % len(multi))
    w(u"=" * 78)

    w(u"")
    w(u"【相交却同组】这些路口永远不交替，是卡车的直接原因")
    w(u"-" * 78)
    for c in bad_diff[:25]:
        ct = c["center"]
        w(u"(%7.0f,%8.0f)  %d 段  轴向 %s"
          % (ct.x, ct.y, len(c["segs"]),
             u" / ".join(u"%.0f°×%d段" % (ax["ang"], len(ax["segs"])) for ax in c["axes"])))
        for kind, s1, s2, d in c["probs"][:3]:
            if kind != u"相交却同组":
                continue
            w(u"      %s(%.0f°,组%s)  与  %s(%.0f°,组%s)   夹角 %.0f°"
              % (s1["label"][:30], s1["ang"], s1["ln"],
                 s2["label"][:30], s2["ang"], s2["ln"], d))
    if len(bad_diff) > 25:
        w(u"   …… 还有 %d 个" % (len(bad_diff) - 25))

    if bad_same:
        w(u"")
        w(u"【同轴却异组】对向车道被拆开了，一个方向走另一个方向等")
        w(u"-" * 78)
        for c in bad_same[:15]:
            ct = c["center"]
            w(u"(%7.0f,%8.0f)  %d 段" % (ct.x, ct.y, len(c["segs"])))
            for kind, s1, s2, d in c["probs"][:3]:
                if kind != u"同轴却异组":
                    continue
                w(u"      %s(%.0f°,组%s)  与  %s(%.0f°,组%s)   夹角仅 %.0f°"
                  % (s1["label"][:30], s1["ang"], s1["ln"],
                     s2["label"][:30], s2["ang"], s2["ln"], d))

    if multi:
        w(u"")
        w(u"【方向多于两个】三岔及以上，两个灯组表达不了，需要单独设计")
        w(u"-" * 78)
        for c in multi[:12]:
            ct = c["center"]
            w(u"(%7.0f,%8.0f)  轴向 %s" % (ct.x, ct.y,
              u" / ".join(u"%.0f°" % ax["ang"] for ax in c["axes"])))

    w(u"")
    w(u"=" * 78)
    w(u"  · 根因在 gen_traffic_lanes.py 的 set_light_number：它按 |Δy| vs |Δx|")
    w(u"    judged沿 X 还是沿 Y，**用的是世界坐标轴、不是两条路的相对关系**。")
    w(u"    自由角度的路网上，两条互相垂直的斜路可能都被判到同一边。")
    w(u"  · 正确做法是**在路口内部做二染色**：把该路口的段按轴角聚成两类，")
    w(u"    一类给 0、一类给 1。这是局部决策，和世界坐标轴无关。")
    w(u"  · 要我改生成器 + 写个就地修正现有值的脚本，说一声。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[axes] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[axes] 写入 %s" % OUT)
