# -*- coding: utf-8 -*-
"""查关卡里的车有没有重叠（或挨得过近），可选自动挪开。

两辆车叠在一起会出两种毛病：视觉上穿模；以及一出生就互相判定为前车，
前车避让把双方速度都压到 0，整组卡死不动（有 MaxBlockedTime 超时兜底，
但那是在测避让不是在测跟线）。

为什么会叠：车是由几个脚本分别放的——spawn_test_cars 按车道挑位置、
add_more_cars 按路口补，两者不知道对方放在哪。同一条车道的起点附近
就可能被放两次。

判据不看碰撞盒（各车型盒子大小不一，而且缩放过），直接用**中心距**：
    < MIN_GAP        算重叠，要处理
    < WARN_GAP       挨得近，只报告
FIX=True 时把每组重叠里除第一辆之外的**删掉**——挪开的话没地方保证
它还落在车道上、Box 还在前方，删掉比乱挪安全。

用法：py check_car_overlap.py
"""

import traceback

import unreal

NAME_PREFIX = "BP_car_base"
MIN_GAP = 400.0          # 中心距小于这个算重叠。车长目测 400~500，缩放 0.7 后约 300
WARN_GAP = 800.0         # 小于这个只报告，不处理
FIX = True               # True = 删掉重叠组里多余的车
Z_TOLERANCE = 1500.0     # Z 差超过这个就不算同一处（上下两层路）

OUT = unreal.Paths.project_saved_dir() + "car_overlap.txt"
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
    cars = []
    for a in eas.get_all_level_actors():
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        if not str(a.get_class().get_name()).startswith(NAME_PREFIX):
            continue
        cars.append((a.get_actor_label(), a.get_actor_location(),
                     a.get_class().get_name(), a))

    w("车 %d 辆" % len(cars))
    w("重叠判据：中心距 < %.0f（且 Z 差 < %.0f）；%.0f 以内只报告"
      % (MIN_GAP, Z_TOLERANCE, WARN_GAP))
    w("")
    if len(cars) < 2:
        w("少于两辆，没什么可查的。")
        flush()
        return

    pairs = []
    for i in range(len(cars)):
        for j in range(i + 1, len(cars)):
            _la, pa, _ca, _aa = cars[i]
            _lb, pb, _cb, _ab = cars[j]
            if abs(pa.z - pb.z) > Z_TOLERANCE:
                continue
            d = ((pa.x - pb.x) ** 2 + (pa.y - pb.y) ** 2
                 + (pa.z - pb.z) ** 2) ** 0.5
            if d < WARN_GAP:
                pairs.append((d, i, j))
    pairs.sort()

    over = [p for p in pairs if p[0] < MIN_GAP]
    near = [p for p in pairs if p[0] >= MIN_GAP]

    w("=" * 92)
    w("① 重叠（中心距 < %.0f）：%d 对" % (MIN_GAP, len(over)))
    w("=" * 92)
    for d, i, j in over:
        w("  %7.0f cm   %-22s %-18s  <->  %-22s %s"
          % (d, cars[i][0][:22], cars[i][2][:18],
             cars[j][0][:22], cars[j][2][:18]))
    if not over:
        w("  没有。")

    w("")
    w("=" * 92)
    w("② 挨得近（%.0f ~ %.0f）：%d 对——不处理，但起步时可能互相避让"
      % (MIN_GAP, WARN_GAP, len(near)))
    w("=" * 92)
    for d, i, j in near[:20]:
        w("  %7.0f cm   %-22s  <->  %s"
          % (d, cars[i][0][:22], cars[j][0][:22]))
    if len(near) > 20:
        w("  ... 还有 %d 对" % (len(near) - 20))
    if not near:
        w("  没有。")

    # --- 处理：每组重叠只留一辆 ---
    killed = 0
    if over and FIX:
        # 并查集把互相重叠的连成组，组里只留下标最小的那辆
        parent = list(range(len(cars)))

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        for _d, i, j in over:
            ri, rj = find(i), find(j)
            if ri != rj:
                parent[max(ri, rj)] = min(ri, rj)

        groups = {}
        for i in range(len(cars)):
            if any(i in (a, b) for _d, a, b in over):
                groups.setdefault(find(i), []).append(i)

        w("")
        w("=" * 92)
        w("处理：每组重叠只留一辆")
        w("=" * 92)
        for root in sorted(groups):
            members = sorted(groups[root])
            keep = members[0]
            w("  保留 %-22s，删掉 %s"
              % (cars[keep][0][:22],
                 "、".join(cars[m][0][:22] for m in members[1:])))
            for m in members[1:]:
                try:
                    eas.destroy_actor(cars[m][3])
                    killed += 1
                except Exception as exc:
                    w("    !! 删不掉 %s：%s" % (cars[m][0], str(exc)[:40]))

    w("")
    w("=" * 92)
    w("合计")
    w("=" * 92)
    w("车 %d 辆，重叠 %d 对，挨得近 %d 对" % (len(cars), len(over), len(near)))
    if FIX:
        w("已删掉重复的 %d 辆，剩 %d 辆" % (killed, len(cars) - killed))
    else:
        w("FIX=False，只报告没动。")
    w("")
    w("删而不是挪：挪开就没法保证它还落在车道上、Box 还在它前方，")
    w("那样车照样接不上路，只是毛病从「叠在一起」变成「看不出为什么不动」。")
    w("要补回数量就再跑一次 add_more_cars.py，它会挑还没有车的路口。")
    w("关卡尚未保存。")
    flush()
    unreal.log("[overlap] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[overlap] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
