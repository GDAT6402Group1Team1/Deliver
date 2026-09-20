# -*- coding: utf-8 -*-
"""Simulate 期间让编辑器视口相机跟拍一辆车（第三人称跟车视角）。

比 "Lock Viewport to Actor" 好用的地方：那个是刚性绑定车的 transform，
车一转弯画面整个甩过去，很晕；这里只跟位置、朝向自己算，
距离/高度/平滑都能调。

用每帧回调驱动，不是 sleep 轮询——Python 跑在游戏线程上，轮询会卡住模拟。

再跑一次本脚本就是停止（切换式），不用另开一个停止脚本。
按不按 Simulate 之前跑都行，它会等游戏世界出现。

用法：py chase_car.py
"""

import time
import traceback

import unreal

TARGET = "car_base4"     # 模糊匹配，忽略下划线和大小写
DISTANCE = 900.0         # 相机在车后多远
HEIGHT = 350.0           # 相机在车上方多高
LOOK_AHEAD = 200.0       # 注视点比车身再往前一点，视野里多留些前方的路
SMOOTH = 0.15            # 0~1，每帧向目标位置靠拢的比例。小=更平滑更滞后
DURATION = 600.0         # 跑这么久（秒）自动停

KEY = "_delivery_chase_cam"


def norm(s):
    return s.lower().replace("_", "")


def find_car(world):
    try:
        allc = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor)
    except Exception:
        return None, []
    want = norm(TARGET)
    names = []
    for a in allc:
        try:
            nm, lbl = a.get_name(), a.get_actor_label()
        except Exception:
            continue
        if "car" in norm(nm) or "car" in norm(lbl):
            names.append("%s / %s" % (nm, lbl))
            if want in norm(nm) or want in norm(lbl):
                return a, names
    return None, names


def tick(_delta):
    st = getattr(unreal, KEY, None)
    if st is None:
        return
    if time.time() - st["t0"] > DURATION:
        stop("到时自动停")
        return

    try:
        world = unreal.get_editor_subsystem(
            unreal.UnrealEditorSubsystem).get_game_world()
    except Exception:
        world = None
    if world is None:
        return                      # 还没按 Simulate，继续等

    car = st.get("car")
    if car is None:
        car, names = find_car(world)
        if car is None:
            unreal.log_warning("[chase] 找不到 '%s'。带 car 的 actor: %s"
                               % (TARGET, "; ".join(names[:10])))
            stop("找不到目标")
            return
        st["car"] = car
        unreal.log("[chase] 跟拍 %s" % car.get_actor_label())

    try:
        loc = car.get_actor_location()
        fwd = car.get_actor_forward_vector()
    except Exception:
        stop("目标已失效")
        return

    # 目标机位：车后 DISTANCE、上方 HEIGHT。只用水平前向，
    # 否则车身俯仰会让相机忽上忽下。
    h = (fwd.x * fwd.x + fwd.y * fwd.y) ** 0.5
    fx, fy = (fwd.x / h, fwd.y / h) if h > 1e-3 else (1.0, 0.0)
    want = unreal.Vector(loc.x - fx * DISTANCE,
                         loc.y - fy * DISTANCE,
                         loc.z + HEIGHT)

    cur = st.get("cam")
    if cur is None:
        cur = want                  # 第一帧直接就位，不要从原点飞过来
    else:
        cur = unreal.Vector(cur.x + (want.x - cur.x) * SMOOTH,
                            cur.y + (want.y - cur.y) * SMOOTH,
                            cur.z + (want.z - cur.z) * SMOOTH)
    st["cam"] = cur

    look = unreal.Vector(loc.x + fx * LOOK_AHEAD,
                         loc.y + fy * LOOK_AHEAD, loc.z)
    try:
        rot = unreal.MathLibrary.find_look_at_rotation(cur, look)
        unreal.get_editor_subsystem(
            unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(cur, rot)
    except Exception as exc:
        unreal.log_error("[chase] 设相机失败: %s" % str(exc)[:90])
        stop("设相机失败")


def stop(why):
    st = getattr(unreal, KEY, None)
    if st is None:
        return
    if st.get("handle") is not None:
        try:
            unreal.unregister_slate_post_tick_callback(st["handle"])
        except Exception:
            pass
    try:
        delattr(unreal, KEY)
    except Exception:
        pass
    unreal.log("[chase] 已停止（%s）" % why)


try:
    if getattr(unreal, KEY, None) is not None:
        stop("再次运行 = 停止")
    else:
        st = {"t0": time.time(), "car": None, "cam": None, "handle": None}
        setattr(unreal, KEY, st)
        st["handle"] = unreal.register_slate_post_tick_callback(tick)
        unreal.log("[chase] 已挂上，按 Simulate 就会开始跟拍 %s。"
                   "再跑一次本脚本即停止。" % TARGET)
except Exception:
    unreal.log_error("[chase] fail:" + chr(10) + traceback.format_exc())
