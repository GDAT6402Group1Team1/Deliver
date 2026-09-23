# -*- coding: utf-8 -*-
"""给还没有车的路口各补一辆车，顺便清掉之前排的车队。

改过一版：上一版是在每辆车后面排 2 辆组成车队（标签 _q1/_q2），
测的是前车避让；现在要的是**覆盖更多路口**，所以那些队尾车先删掉。

选法：每条车道段的**终点**都对着一个路口（车沿这条道开过去就进那个路口）。
按终点把车道归到最近的路口上，统计哪些路口已经有车在往它开，
给没车的路口各挑一条车道补一辆。

车摆在车道段 Box **后方** BEHIND_BOX 处：Box 挂在第一个控制点上，
车靠 TraceForNewPath 往前扫才拿得到样条，摆在 Box 前面等于一出生就错过它。

用法：py add_more_cars.py
"""

import math
import random
import traceback

import unreal

VEHICLE_DIR = "/Game/Vehicle"
NAME_PREFIX = "BP_car_base"
LANE_TAG_PREFIX = "ClaudeGenLane"
INTER_LABEL_PREFIX = "Intersection_"

REMOVE_QUEUE_SUFFIX = "_q"   # 上一版车队车的标签后缀，先清掉
MAX_NEW = 40                 # 最多补几辆
FILL_EMPTY_LANES = True      # True = 不只按路口补，还把"附近没车"的车道段铺开。
                             # 按路口补每个路口只放一辆，覆盖不了长路中段；
                             # 要的是各条交通线上都有车，就得按车道段来。
LANE_STRIDE = 2              # 空车道每隔几条取一条，避免整条路挤满
LANE_BUSY_RADIUS = 1200.0    # 车道起点这么近有车就算这条道已经有车了
FORCE_ADD = ["Intersection_12"]   # 这些路口无论附近有没有车都补一辆。
                                  # "已经有车"的判据是半径 COVERED_RADIUS 内有车，
                                  # 隔壁路口的车也会把它算成已覆盖，所以要能手动指定。
BEHIND_BOX = 300.0           # 车摆在 Box 后方多远
JUNCTION_RADIUS = 3500.0     # 车道终点离路口多近才算"通向这个路口"
COVERED_RADIUS = 4000.0      # 现有的车离路口多近就算这个路口已经有车了
MIN_LANE_LEN = 1500.0        # 太短的段不放，车还没加速就到头了

SCALE = 0.7
SCALE_BY_VARIANT = {
    "BP_car_base2": 0.5,
    "BP_car_base3": 0.8,
}
SPEED_MIN, SPEED_MAX, SPEED_STEP = 600, 1500, 100
TAG = "ClaudeTestCar"
SEED = None

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "add_more_cars.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def find_variants():
    out = []
    try:
        paths = unreal.EditorAssetLibrary.list_assets(
            VEHICLE_DIR, recursive=False, include_folder=False)
    except Exception as exc:
        w("!! 列不出 %s：%s" % (VEHICLE_DIR, str(exc)[:60]))
        return out
    for pth in paths:
        name = str(pth).split("/")[-1].split(".")[0]
        if not name.startswith(NAME_PREFIX):
            continue
        asset = unreal.EditorAssetLibrary.load_asset(str(pth).split(".")[0])
        if asset is not None and isinstance(asset, unreal.Blueprint):
            out.append((name, asset))
    out.sort(key=lambda t: t[0])
    return out


def dist2d(a, b):
    return ((a.x - b.x) ** 2 + (a.y - b.y) ** 2) ** 0.5


def run():
    if SEED is not None:
        random.seed(SEED)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # --- 0. 清掉上一版排的车队 ---
    queued = [a for a in actors
              if str(a.get_class().get_name()).startswith(NAME_PREFIX)
              and REMOVE_QUEUE_SUFFIX in a.get_actor_label()]
    killed = 0
    for a in queued:
        try:
            eas.destroy_actor(a)
            killed += 1
        except Exception:
            pass
    w("清掉上一版的车队车 %d 辆（标签里含 '%s'）" % (killed, REMOVE_QUEUE_SUFFIX))
    w("")

    variants = find_variants()
    if not variants:
        w("!! 没找到车型。")
        flush()
        return

    # --- 1. 路口 ---
    inters = [(a.get_actor_label(), a.get_actor_location()) for a in actors
              if a.get_actor_label().startswith(INTER_LABEL_PREFIX)]

    # --- 2. 车道段：记起点（Box）、起点方向、终点（通向哪个路口） ---
    lanes = []
    for a in actors:
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        if not a.get_actor_label().startswith("Lane_"):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        L = sp.get_spline_length()
        if L < MIN_LANE_LEN:
            continue
        p = sp.get_location_at_distance_along_spline(0.0, WS)
        q = sp.get_location_at_distance_along_spline(min(100.0, L), WS)
        e = sp.get_location_at_distance_along_spline(L, WS)
        dx, dy = q.x - p.x, q.y - p.y
        h = (dx * dx + dy * dy) ** 0.5
        if h < 1e-3:
            continue
        lanes.append((a.get_actor_label(), p, dx / h, dy / h, e))

    # --- 3. 现有的车（清完队列之后）覆盖了哪些路口 ---
    cars = [a for a in eas.get_all_level_actors()
            if str(a.get_class().get_name()).startswith(NAME_PREFIX)]
    covered = set()
    for c in cars:
        loc = c.get_actor_location()
        for nm, ip in inters:
            if dist2d(loc, ip) < COVERED_RADIUS:
                covered.add(nm)

    w("路口 %d 个，车道段 %d 条，现有的车 %d 辆"
      % (len(inters), len(lanes), len(cars)))
    w("已经有车在附近的路口 %d 个，没车的 %d 个"
      % (len(covered), len(inters) - len(covered)))
    w("")
    if not inters or not lanes:
        w("!! 缺路口或车道段。")
        flush()
        return

    # --- 4. 每条车道按终点归到最近的路口 ---
    by_junction = {}
    for lbl, p, ux, uy, e in lanes:
        best = None
        for nm, ip in inters:
            d = dist2d(e, ip)
            if d < JUNCTION_RADIUS and (best is None or d < best[0]):
                best = (d, nm)
        if best is not None:
            by_junction.setdefault(best[1], []).append((lbl, p, ux, uy))

    # --- 4b. 按车道段铺开：找"起点附近没有车"的车道 ---
    free_lanes = []
    if FILL_EMPTY_LANES:
        for lbl, p, ux, uy, _e in lanes:
            busy = False
            for c in cars:
                cl = c.get_actor_location()
                if ((cl.x - p.x) ** 2 + (cl.y - p.y) ** 2) ** 0.5 < LANE_BUSY_RADIUS:
                    busy = True
                    break
            if not busy:
                free_lanes.append((lbl, p, ux, uy))
        free_lanes.sort(key=lambda t: t[0])
        free_lanes = free_lanes[::LANE_STRIDE]
        w("附近没车的车道段 %d 条（每 %d 条取一条后剩 %d 条）"
          % (len([1 for lbl, p, ux, uy, _e in lanes]), LANE_STRIDE,
             len(free_lanes)))

    forced = [n.strip() for n in FORCE_ADD if n.strip()]
    todo = [nm for nm, _ip in inters
            if nm not in covered and nm in by_junction]
    # 强制名单排在最前，保证不会被 MAX_NEW 截掉
    head = [n for n in forced if n in by_junction and n not in todo]
    todo = head + todo
    w("没车、且有车道通向它的路口 %d 个，最多补 %d 辆"
      % (len(todo) - len(head), MAX_NEW))
    if head:
        w("强制补的路口 %d 个：%s" % (len(head), "、".join(head)))
    missing = [n for n in forced if n not in by_junction]
    if missing:
        w("!! 强制名单里这几个补不了：%s" % "、".join(missing))
        w("   要么关卡里没有这个路口，要么 %.0f 范围内没有终点指向它的车道段"
          % JUNCTION_RADIUS)
        w("   （路口存在但周围只有路口段、没有普通车道段时就会这样）。")
    w("")
    flush()

    # 车速轮转整个区间再打乱：纯随机在十几辆的规模上很容易扎堆
    #（实测上一轮 12 辆里 BP_car_base4 占了 5 辆、1400 出现两次），
    # 轮转能保证 600~1500 每档都出现过。
    all_speeds = list(range(SPEED_MIN, SPEED_MAX + 1, SPEED_STEP))
    pool = []
    while len(pool) < MAX_NEW + len(todo) + 8:
        batch = list(all_speeds)
        random.shuffle(batch)
        pool.extend(batch)
    speed_iter = iter(pool)
    w("%-16s %-30s %-18s %7s %5s %s"
      % ("路口", "车道段", "车型", "车速", "缩放", "备注"))
    w("-" * 100)

    made = 0
    used = {}
    speeds_used = {}
    for nm in todo:
        if made >= MAX_NEW:
            break
        lbl, p, ux, uy = random.choice(by_junction[nm])
        name, bp = random.choice(variants)
        yaw = math.degrees(math.atan2(uy, ux))
        loc = unreal.Vector(p.x - ux * BEHIND_BOX,
                            p.y - uy * BEHIND_BOX, p.z)
        try:
            car = eas.spawn_actor_from_object(
                bp, loc, unreal.Rotator(0.0, 0.0, yaw))   # (roll, pitch, yaw)
        except Exception as exc:
            w("%-16s %-30s %-18s  !! spawn 失败：%s"
              % (nm[:16], lbl[:30], name[:18], str(exc)[:36]))
            continue
        if car is None:
            continue
        sc = SCALE_BY_VARIANT.get(name, SCALE)
        spd = next(speed_iter)
        note = ""
        try:
            car.set_actor_scale3d(unreal.Vector(sc, sc, sc))
        except Exception:
            note += "缩放失败 "
        try:
            car.modify(True)
            car.set_editor_property("MaxSpeed", float(spd))
        except Exception:
            spd = -1
            note += "MaxSpeed 写不进去 "
        try:
            car.set_actor_label("TestCar_%s" % nm.replace(INTER_LABEL_PREFIX, "I"))
            tags = list(car.tags)
            tags.append(TAG)
            car.set_editor_property("tags", tags)
        except Exception:
            pass
        used[name] = used.get(name, 0) + 1
        speeds_used[spd] = speeds_used.get(spd, 0) + 1
        made += 1
        w("%-16s %-30s %-18s %7s %5.2f %s"
          % (nm[:16], lbl[:30], name[:18],
             ("%d" % spd) if spd > 0 else "失败", sc, note))
        flush()

    # --- 6. 路口都覆盖完还有名额，就继续按车道段铺 ---
    if FILL_EMPTY_LANES and made < MAX_NEW:
        w("")
        w("--- 按车道段继续铺（路口已覆盖，还剩 %d 个名额）---" % (MAX_NEW - made))
        for lbl, p, ux, uy in free_lanes:
            if made >= MAX_NEW:
                break
            name, bp = random.choice(variants)
            yaw = math.degrees(math.atan2(uy, ux))
            loc = unreal.Vector(p.x - ux * BEHIND_BOX,
                                p.y - uy * BEHIND_BOX, p.z)
            try:
                car = eas.spawn_actor_from_object(
                    bp, loc, unreal.Rotator(0.0, 0.0, yaw))
            except Exception:
                continue
            if car is None:
                continue
            sc = SCALE_BY_VARIANT.get(name, SCALE)
            spd = next(speed_iter)
            try:
                car.set_actor_scale3d(unreal.Vector(sc, sc, sc))
            except Exception:
                pass
            try:
                car.modify(True)
                car.set_editor_property("MaxSpeed", float(spd))
            except Exception:
                spd = -1
            try:
                car.set_actor_label("TestCar_%s" % lbl.replace("Lane_", ""))
                tags = list(car.tags)
                tags.append(TAG)
                car.set_editor_property("tags", tags)
            except Exception:
                pass
            used[name] = used.get(name, 0) + 1
            speeds_used[spd] = speeds_used.get(spd, 0) + 1
            made += 1
            w("%-16s %-30s %-18s %7s %5.2f"
              % ("(车道)", lbl[:30], name[:18],
                 ("%d" % spd) if spd > 0 else "失败", sc))
        flush()

    w("")
    w("=" * 100)
    w("清掉车队车 %d 辆，新补 %d 辆" % (killed, made))
    w("车速分布：%s"
      % "，".join("%d×%d" % kv for kv in sorted(speeds_used.items())))
    w("车型分布：%s" % ("，".join("%s × %d" % kv for kv in sorted(used.items()))
                      or "（没加）"))
    w("")
    w("每辆都摆在所选车道段 Box 后方 %.0f 处、朝向对齐车道，" % BEHIND_BOX)
    w("沿这条道开过去就进对应的那个路口。")
    w("关卡尚未保存。")
    flush()
    unreal.log("[addcar] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[addcar] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
