# -*- coding: utf-8 -*-
"""把关卡里已经摆好的车随机换成 /Game/Vehicle 下的各种车型，按车型设缩放。

保留原来的**位置和朝向**——那些是按车道起点、盒子前方算好的，重新挑一遍
只会把已经调好的布局打乱。只换类、设缩放、把原来的 MaxSpeed 搬过去。

蓝图类是不能就地替换的（actor 的类在 spawn 时就定死了），所以流程是
"记下变换 -> 删掉 -> 用新类在同一个变换上重新 spawn"。

MaxSpeed 要搬：它是纯配置变量、整张蓝图没有 Set 节点，新车型的默认值
多半和你之前随机出来的那批不一样，不搬的话之前"车速各不相同"就没了。
搬不到（变体上没这个变量）时按 SPEED_MIN~MAX 重新随机，并在报告里点名。

用法：py randomize_cars.py
"""

import math
import random
import traceback

import unreal

VEHICLE_DIR = "/Game/Vehicle"      # 在这里找车型，不递归（Motorbike 在子目录里，自动排除）
NAME_PREFIX = "BP_car_base"        # 只认名字以此开头的蓝图
CAR_CLASS_SUFFIX = "_C"
SCALE = 0.7                        # 默认缩放（车型没在下面单列时用这个）
SCALE_BY_VARIANT = {               # 个别车型的模型本身大小不一样，单独给值
    "BP_car_base2": 0.5,
    "BP_car_base3": 0.8,
}
KEEP_SPEED = True                  # True = 把旧车的 MaxSpeed 搬到新车上
SPEED_MIN, SPEED_MAX, SPEED_STEP = 600, 1500, 100   # 搬不到时重新随机的范围
TAG = "ClaudeTestCar"
SEED = None                        # 填个整数可复现同一套随机结果

OUT = unreal.Paths.project_saved_dir() + "randomize_cars.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def is_car_class(cls_name):
    return (str(cls_name).startswith(NAME_PREFIX)
            and str(cls_name).endswith(CAR_CLASS_SUFFIX))


def find_variants():
    """列出 VEHICLE_DIR 下所有车型蓝图（不递归）。"""
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
        if asset is None or not isinstance(asset, unreal.Blueprint):
            continue
        out.append((name, asset))
    out.sort(key=lambda t: t[0])
    return out


def run():
    if SEED is not None:
        random.seed(SEED)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    variants = find_variants()
    w("车型 %d 种（%s，不递归）" % (len(variants), VEHICLE_DIR))
    for n, _a in variants:
        w("   %s" % n)
    w("")
    if not variants:
        w("!! 一种都没找到。检查路径和 NAME_PREFIX。")
        flush()
        return

    # 收集现有的车。按**类名**认，不按 actor 标签——标签五花八门。
    olds = []
    for a in eas.get_all_level_actors():
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        if not is_car_class(a.get_class().get_name()):
            continue
        spd = None
        try:
            spd = a.get_editor_property("MaxSpeed")
        except Exception:
            pass
        olds.append((a.get_actor_label(), a.get_class().get_name(),
                     a.get_actor_location(), a.get_actor_rotation(),
                     list(a.tags), spd, a))

    w("关卡里现有的车 %d 辆" % len(olds))
    if not olds:
        w("!! 一辆都没有。先跑 spawn_test_cars.py 摆一批。")
        flush()
        return
    by_cls = {}
    for _l, c, _p, _r, _t, _s, _a in olds:
        by_cls[c] = by_cls.get(c, 0) + 1
    w("   按类分布：%s" % "，".join("%s × %d" % kv for kv in by_cls.items()))
    w("")
    flush()

    speeds = list(range(SPEED_MIN, SPEED_MAX + 1, SPEED_STEP))
    w("%-4s %-16s %-18s %-26s %7s %7s %5s %s"
      % ("#", "原车型", "新车型", "位置", "朝向", "车速", "缩放", "备注"))
    w("-" * 118)

    made = fail = 0
    speed_lost = 0
    used = {}
    for i, (lbl, oldcls, loc, rot, tags, spd, actor) in enumerate(olds):
        name, bp = random.choice(variants)
        # 先删再放：actor 的类在 spawn 时定死，没法就地换
        try:
            eas.destroy_actor(actor)
        except Exception as exc:
            w("%-4d !! 删不掉 %s：%s" % (i, lbl[:24], str(exc)[:40]))
            fail += 1
            continue
        try:
            car = eas.spawn_actor_from_object(bp, loc, rot)
        except Exception as exc:
            w("%-4d !! spawn 失败 %s：%s" % (i, name, str(exc)[:40]))
            fail += 1
            continue
        if car is None:
            w("%-4d !! spawn 返回 None（%s）" % (i, name))
            fail += 1
            continue

        note = ""
        sc = SCALE_BY_VARIANT.get(name, SCALE)
        try:
            car.set_actor_scale3d(unreal.Vector(sc, sc, sc))
        except Exception as exc:
            note += "缩放失败(%s) " % str(exc)[:24]

        newspd = spd if (KEEP_SPEED and spd) else random.choice(speeds)
        try:
            car.modify(True)
            car.set_editor_property("MaxSpeed", float(newspd))
        except Exception:
            newspd = -1
            speed_lost += 1
            note += "MaxSpeed 写不进去 "

        try:
            car.set_actor_label(lbl)      # 沿用原来的名字，视口里对得上
            keep = [t for t in tags if str(t)]
            if TAG not in [str(t) for t in keep]:
                keep.append(TAG)
            car.set_editor_property("tags", keep)
        except Exception:
            pass

        used[name] = used.get(name, 0) + 1
        made += 1
        w("%-4d %-16s %-18s (%7.0f,%8.0f,%7.0f) %7.1f %7s %5.2f %s"
          % (i, oldcls[:16], name[:18], loc.x, loc.y, loc.z,
             rot.yaw, ("%d" % newspd) if newspd > 0 else "失败", sc, note))
        if i % 10 == 0:
            flush()

    w("")
    w("=" * 118)
    w("替换 %d 辆，失败 %d 辆" % (made, fail))
    w("缩放：默认 %.2f，单列的 %s"
      % (SCALE, "，".join("%s=%.2f" % kv for kv in sorted(SCALE_BY_VARIANT.items()))))
    w("车型分布：%s" % "，".join("%s × %d" % kv for kv in sorted(used.items())))
    if speed_lost:
        w("!! %d 辆的 MaxSpeed 没写进去——那几个变体蓝图可能没有这个变量，"
          % speed_lost)
        w("   或者名字不叫 MaxSpeed。那些车会用变体自己的默认车速。")
    w("")
    w("位置和朝向沿用原车，没有重新挑车道——那些落点是按「车道起点往后退、")
    w("让 Box 落在车前方」算好的，重挑会把它打乱。")
    w("关卡尚未保存。")
    flush()
    unreal.log("[rndcar] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[rndcar] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
