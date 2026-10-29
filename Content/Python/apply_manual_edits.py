# -*- coding: utf-8 -*-
"""按 Design/TrafficManualEdits.json 重新应用路口样条的重合关系。

用途：全图重建之后，把人工调过的"三条路口线怎么重合"恢复回去。
放在生成链**最后一步**：

    gen_traffic_lanes → gen_intersections → fix_light_number_by_axis
      →（另起执行）fill_turn_splines →（另起执行）fix_t_junctions
      → **apply_manual_edits**

**刻意不去改那两个生成器**：它们的主流程复杂（COLLAPSE_ALL_FIRST 会先把全图
转弯压平、再按几何重画），插钩子容易出意外；而"最后再覆盖一遍"是幂等的、
出错也只影响这一步。

匹配靠 **路名 + 偏移 + 坐标**，不靠 `I` 序号——序号在重建后会变位
（实测路口编号重排过：原 Intersection_40 变成了 34），按序号匹配会对错位置。
同一条路同一偏移下取**最近**的那个段，且必须在 MATCH_DIST 以内，
超出就报"没匹配上"而不是硬套。

`=<另一条>` 的链会先解析到最终源头（比如 Right=Left、Left=Spline，
则 Right 实际要复制 Spline），所以写入顺序无所谓。

DRY_RUN=True 只报不改（默认）。
用法：py apply_manual_edits.py
"""

import json
import math
import re
import traceback

import unreal

DRY_RUN = True
MATCH_DIST = 1500.0      # 路名+偏移相同的前提下，坐标差在此以内算同一个段
INTER_RE = re.compile(r'^Inter_(.+)_([+-]\d+)_I(\d+)$')

JSON_IN = unreal.Paths.project_dir() + "Design/TrafficManualEdits.json"
OUT = unreal.Paths.project_saved_dir() + "apply_manual_edits.txt"
WS = unreal.SplineCoordinateSpace.WORLD
lines = []
_mark_failed = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[apply] %s" % s)


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


def dedup_pts(pts, eps=2.0):
    out = []
    for p in pts:
        if out:
            q = out[-1]
            if ((p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2) ** 0.5 < eps:
                continue
        out.append(p)
    return out


def pts_of(sp):
    return [sp.get_location_at_spline_point(i, WS)
            for i in range(sp.get_number_of_spline_points())]


def write_spline(sp, pts):
    """写入顺序和 fill_turn_splines.py 一致，两条铁律见那里的注释：
    mark_edited 必须在写点之前（否则组件被构造脚本重建、点落在被丢弃的那批上），
    modify(True) 也必须在改动之前（否则存盘时整个对象不被序列化）。"""
    marked = mark_edited(sp)
    try:
        sp.modify(True)
        owner = sp.get_owner()
        if owner is not None:
            owner.modify(True)
    except Exception:
        pass
    sp.clear_spline_points(False)
    for p in dedup_pts(pts):
        sp.add_spline_point(p, WS, False)
    n = sp.get_number_of_spline_points()
    for i in range(n):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
    sp.update_spline()
    if not marked:
        _mark_failed.append(sp.get_name())
    return n


def same_points(a, b, eps=5.0):
    pa, pb = pts_of(a), pts_of(b)
    if len(pa) != len(pb) or not pa:
        return False
    for p, q in zip(pa, pb):
        if ((p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2) ** 0.5 > eps:
            return False
    return True


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 重新应用路口样条重合关系 ===")
    w(u"关卡：%s     模式：%s"
      % (world.get_name() if world else u"?", u"只报不改" if DRY_RUN else u"**写入**"))

    try:
        with open(JSON_IN, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except Exception as exc:
        w(u"！读不到 %s：%s" % (JSON_IN, str(exc)[:90]))
        w(u"  先跑 export_manual_edits.py 导出一份。")
        return
    recs = data.get("intersection_splines") or []
    w(u"清单里有 %d 个路口段的记录" % len(recs))
    if not recs:
        w(u"！清单里没有 intersection_splines。导出脚本是旧版？重跑一次 export。")
        return

    # 现场的所有 Inter_ 段，按 (路名, 偏移) 归类
    live = {}
    for a in eas.get_all_level_actors():
        m = INTER_RE.match(a.get_actor_label())
        if not m:
            continue
        sd = {c.get_name(): c for c in a.get_components_by_class(unreal.SplineComponent)}
        if not sd:
            continue
        main = None
        for k in ("Spline", "SplineComponent"):
            if k in sd:
                main = k
                break
        if main is None:
            continue
        p0 = pts_of(sd[main])
        if not p0:
            continue
        live.setdefault((m.group(1), m.group(2)), []).append(
            {"a": a, "label": a.get_actor_label(), "sd": sd, "p0": p0[0]})
    w(u"现场 Inter_ 段 %d 个" % sum(len(v) for v in live.values()))

    def resolve(ident, nm, depth=0):
        """把 =A、A=B 这种链解析到最终源头。"""
        v = ident.get(nm, u"unique")
        if v == u"unique" or depth > 4:
            return None
        return resolve(ident, v[1:], depth + 1) or v[1:]

    applied, already, nomatch, missing = 0, 0, [], []
    for r in recs:
        key = (r.get("road"), r.get("off"))
        cands = live.get(key) or []
        best, bd = None, 1e18
        for c in cands:
            d = math.sqrt((c["p0"].x - r.get("x", 0)) ** 2
                          + (c["p0"].y - r.get("y", 0)) ** 2)
            if d < bd:
                best, bd = c, d
        if best is None or bd > MATCH_DIST:
            nomatch.append(u"%s %s I%s @ (%.0f,%.0f)%s"
                           % (r.get("road"), r.get("off"), r.get("i"),
                              r.get("x", 0), r.get("y", 0),
                              u"" if best is None
                              else u"  最近的在 %.0f cm 外" % bd))
            continue

        ident = r.get("splines") or {}
        for nm, val in sorted(ident.items()):
            if val == u"unique":
                continue
            src = resolve(ident, nm)
            if src is None or src not in best["sd"] or nm not in best["sd"]:
                missing.append(u"%s · %s（要复制 %s）" % (best["label"], nm, src))
                continue
            if same_points(best["sd"][nm], best["sd"][src]):
                already += 1
                continue
            w(u"  (%8.0f,%9.0f) %-36s %s ← 复制 %s（原 %d 点 / %.0f cm）"
              % (best["p0"].x, best["p0"].y, best["label"][:36], nm, src,
                 best["sd"][nm].get_number_of_spline_points(),
                 best["sd"][nm].get_spline_length()))
            if not DRY_RUN:
                write_spline(best["sd"][nm], pts_of(best["sd"][src]))
            applied += 1

    w(u"")
    w(u"=" * 74)
    w(u"要写 %d 条；本来就一致 %d 条" % (applied, already))
    if nomatch:
        w(u"")
        w(u"**没匹配上 %d 个路口段**（路名+偏移对得上但坐标差太远，或者这条路没了）：" % len(nomatch))
        for x in nomatch[:12]:
            w(u"    %s" % x)
        if len(nomatch) > 12:
            w(u"    …… 还有 %d 个" % (len(nomatch) - 12))
        w(u"  正常情况：那条路被排除了（装饰路），或者路口数变了导致段数变少。")
        w(u"  不正常：坐标差几千——说明重建后几何变了，这份清单可能已经过期。")
    if missing:
        w(u"")
        w(u"要复制的源样条不存在 %d 处：%s" % (len(missing), u"；".join(missing[:5])))
    w(u"")
    if DRY_RUN:
        w(u"只报不改。确认上面的匹配和复制方向都对之后，把 DRY_RUN 改 False 再跑。")
    else:
        w(u"**记得 Ctrl+S 保存关卡。**")
        if _mark_failed:
            w(u"！%d 条没打上已编辑标记，点可能被构造脚本冲回默认值：%s"
              % (len(_mark_failed), u"、".join(_mark_failed[:6])))
        w(u"复核：再跑一次本脚本，应该报「要写 0 条」。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[apply] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[apply] 写入 %s" % OUT)
