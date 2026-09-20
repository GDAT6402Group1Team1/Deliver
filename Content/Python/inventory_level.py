# -*- coding: utf-8 -*-
"""清点关卡里的 actor，查"红绿灯没了"到底少了什么。

只读，不删不改不生成。

四件事：
  1. 按类统计全部 actor，单独把 trafficlight / Intersection / TrafficLine 拎出来
  2. 列出所有 BP_Intersection 实例，区分"我脚本生成的"（带 ClaudeGenIntersection tag）
     和"别人摆的"（没有这个 tag）
  3. 查附着关系：如果红绿灯是挂在 Lane_/Inter_ 底下的子 actor，
     父 actor 被删时可能被一起带走——这是目前最可疑的一条路径
  4. 报告关卡有没有未保存的改动（没保存的话不存盘重开就能全回来）

用法：py inventory_level.py
"""

import traceback

import unreal

KEYWORDS = ["light", "traffic", "inter", "signal", "灯"]

OUT = unreal.Paths.project_saved_dir() + "inventory.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    w("关卡 actor 总数 %d" % len(actors))
    w("")

    # --- 1. 按类统计 ---
    by_cls = {}
    for a in actors:
        try:
            cn = a.get_class().get_name()
        except Exception:
            cn = "?"
        by_cls[cn] = by_cls.get(cn, 0) + 1

    w("=" * 78)
    w("1. 和红绿灯/路口相关的类")
    w("=" * 78)
    hit_any = False
    for cn in sorted(by_cls):
        if any(k in cn.lower() for k in KEYWORDS):
            w("  %-52s %d" % (cn, by_cls[cn]))
            hit_any = True
    if not hit_any:
        w("  一个都没有 —— 这本身就是答案的一部分")
    w("")
    w("  （类名里不含关键词的大类，前 20）")
    for cn in sorted(by_cls, key=lambda c: -by_cls[c])[:20]:
        w("    %-52s %d" % (cn, by_cls[cn]))
    w("")
    flush()

    # --- 2. BP_Intersection 实例 ---
    w("=" * 78)
    w("2. BP_Intersection 实例：我生成的 vs 别人摆的")
    w("=" * 78)
    mine, theirs = [], []
    for a in actors:
        try:
            cn = a.get_class().get_name()
        except Exception:
            continue
        if "Intersection" not in cn:
            continue
        tags = [str(t) for t in a.tags]
        (mine if "ClaudeGenIntersection" in tags else theirs).append((a, tags))
    w("  带 ClaudeGenIntersection tag（我生成的）: %d 个" % len(mine))
    for a, _t in mine[:10]:
        loc = a.get_actor_location()
        sc = a.get_actor_scale3d()
        w("    %-24s @ (%7.0f,%7.0f,%7.0f)  缩放 (%.2f, %.2f, %.2f)"
          % (a.get_actor_label()[:24], loc.x, loc.y, loc.z, sc.x, sc.y, sc.z))
    w("  没有这个 tag（不是我生成的）: %d 个" % len(theirs))
    for a, tags in theirs[:20]:
        loc = a.get_actor_location()
        w("    %-24s @ (%7.0f,%7.0f,%7.0f)  tags=%s"
          % (a.get_actor_label()[:24], loc.x, loc.y, loc.z, tags))
    w("")
    flush()

    # --- 3. 附着关系 ---
    w("=" * 78)
    w("3. 附着关系：有没有东西挂在 Lane_/Inter_ 底下")
    w("=" * 78)
    attached_total = 0
    for a in actors:
        lbl = a.get_actor_label()
        if not (lbl.startswith("Lane_") or lbl.startswith("Inter_")):
            continue
        try:
            kids = a.get_attached_actors()
        except Exception:
            kids = []
        if kids:
            attached_total += len(kids)
            w("  %-30s 挂着 %d 个: %s"
              % (lbl[:30], len(kids),
                 ", ".join(k.get_actor_label()[:20] for k in kids[:4])))
    if attached_total == 0:
        w("  没有任何东西挂在 Lane_/Inter_ 底下。")
        w("  >>> 也就是说'父 actor 被删、子红绿灯被一起带走'这条路径不成立。")
    else:
        w("  >>> 共 %d 个子 actor。这条路径需要进一步查。" % attached_total)
    w("")

    # 反过来：红绿灯类的 actor 有没有父级
    w("  红绿灯类 actor 的父级情况：")
    found = False
    for a in actors:
        try:
            cn = a.get_class().get_name()
        except Exception:
            continue
        if not any(k in cn.lower() for k in ("light", "signal")):
            continue
        found = True
        try:
            par = a.get_attach_parent_actor()
            pl = par.get_actor_label() if par is not None else "（无父级）"
        except Exception:
            pl = "?"
        w("    %-30s 父级 %s" % (a.get_actor_label()[:30], pl))
        if len([1 for _ in ()]) > 0:
            break
    if not found:
        w("    关卡里没有类名带 light/signal 的 actor")
    w("")
    flush()

    # --- 4. 未保存改动 ---
    w("=" * 78)
    w("4. 关卡有没有未保存的改动")
    w("=" * 78)
    try:
        world = unreal.get_editor_subsystem(
            unreal.UnrealEditorSubsystem).get_editor_world()
        pkg = world.get_outermost()
        dirty = pkg.is_dirty() if hasattr(pkg, "is_dirty") else None
        w("  关卡包 %s" % pkg.get_path_name())
        w("  未保存改动: %s" % dirty)
        if dirty:
            w("  >>> 还没存盘。如果确认是误删，关掉关卡选'不保存'再重开，")
            w("      所有东西都会回来（代价是这一轮生成的车道要重跑）。")
    except Exception as exc:
        w("  查不到: %s" % str(exc)[:90])

    w("")
    w("只读脚本，没有删除或修改任何东西。")
    flush()
    unreal.log("[inv] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[inv] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
