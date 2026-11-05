# -*- coding: utf-8 -*-
u"""删掉摆在路口中心的红绿灯。

为什么
------
红绿灯是给**人**看的，该立在路口两侧的人行道上，不是悬在路口正中。
两侧的灯等会儿由队友重新摆，这里只负责把中心那批清掉。

**删灯不影响车停红灯。** 灯和路口是两个独立的定时器（CLAUDE.md 记过：
"已知会漂"）——车读的是 `BP_TrafficLine1_IntersectionChild.StopAtInter`，
由 `BP_Intersection.NextTrafficLight` 每 `LightDuration` 秒翻一次，
整条链上没有任何一步问过灯模型。所以删完之后车照样停、照样走，
只是看不见灯了。**红绿灯的调试球（DrawDebugSphere）也还在**，
PIE 里仍然能看出相位。

判据
----
① 带 `ClaudeGenTrafficLight` tag 的（`gen_traffic_lights.py` 摆的），**和**
② 类名以 `trafficlight` 开头的（手摆的，没有 tag；`trafficlight_green_start`
   那个子类也以它开头，一并覆盖）。

两条都要：只按 tag 清会漏掉手摆的那些，只按类名清会在以后灯换了蓝图时漏掉。
报告里每一盏都打类名、坐标和到最近路口的距离，所以"删掉了什么"是可查的。

用法
----
    py remove_center_traffic_lights.py        # DRY_RUN=True 时只看不删
无界面：
    UnrealEditor-Cmd <工程>.uproject -run=pythonscript \
        -script=<本文件绝对路径> -unattended -nopause -nosplash -NullRHI
输出在 ~/Library/Logs/Unreal Engine/DeliveryEditor/Delivery.log
"""

import math
import traceback

import unreal

DRY_RUN = False
# 处理哪些关卡。逐个 load → 删 → save：编辑器 Python 一次只能操作当前关卡，
# 而且 commandlet 退出时不会替你问要不要保存，save 必须自己调。
LEVELS = [
    u"/Game/Level/TestForCharacter",
    u"/Game/Level/testfortraffic",
]
TAG = u"ClaudeGenTrafficLight"
CLASS_PREFIX = u"trafficlight"

_lines = []


def w(s=u""):
    _lines.append(s)
    unreal.log(s)


def nearest_intersection(loc, inters):
    u"""返回 (距离, 名字)；没有路口时返回 (None, None)。"""
    best = None
    best_name = None
    for a in inters:
        # unreal.Vector 没有 size()，UE5 的 Python API 是 length()；
        # 自己开方不依赖这个名字，换引擎版本也不会炸
        dv = a.get_actor_location() - loc
        d = math.sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z)
        if best is None or d < best:
            best = d
            best_name = a.get_actor_label()
    return best, best_name


def is_light(actor):
    if TAG in [str(t) for t in actor.tags]:
        return u"tag"
    cls = actor.get_class().get_name()
    if cls.lower().startswith(CLASS_PREFIX):
        return u"class:%s" % cls
    return None


def process_level(path):
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(path):
        w(u"！加载不了关卡 %s，跳过。" % path)
        return 0

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    inters = [a for a in actors
              if a and a.get_class().get_name().startswith(u"BP_Intersection")]

    lights = []
    for a in actors:
        if not a:
            continue
        why = is_light(a)
        if why:
            lights.append((a, why))

    w(u"")
    w(u"== %s：%d 个 actor、%d 个路口、%d 盏灯"
      % (path, len(actors), len(inters), len(lights)))

    removed = 0
    for a, why in lights:
        loc = a.get_actor_location()
        d, iname = nearest_intersection(loc, inters)
        dtxt = u"到最近路口 %.0f cm（%s）" % (d, iname) if d is not None else u"图里没有路口"
        w(u"  %s [%s] (%.0f, %.0f, %.0f) %s"
          % (a.get_actor_label(), why, loc.x, loc.y, loc.z, dtxt))
        if not DRY_RUN:
            if eas.destroy_actor(a):
                removed += 1
            else:
                w(u"    ！删不掉，可能被别的东西引用着。")

    if not DRY_RUN and removed:
        les.save_current_level()
        w(u"  已删 %d 盏并保存关卡。" % removed)
    elif DRY_RUN:
        w(u"  DRY_RUN：什么都没动。")
    else:
        w(u"  没有要删的。")
    return removed


def run():
    w(u"=== 删除路口中心红绿灯 ===")
    w(u"DRY_RUN = %s" % DRY_RUN)
    total = 0
    for path in LEVELS:
        total += process_level(path)
    w(u"")
    w(u"合计删除 %d 盏。" % total)
    w(u"提醒：车停红灯读的是 BP_Intersection 翻的 StopAtInter，和灯模型无关，不受影响。")


try:
    run()
except Exception:
    unreal.log_error(traceback.format_exc())
