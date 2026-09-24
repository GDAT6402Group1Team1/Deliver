# -*- coding: utf-8 -*-
"""查关卡里每辆车的缩放淡入设置到底是什么值，按车型分组。

起因：C++ 里 BeginPlay 和 NotifyResetToStart **都**接了缩放淡入，
第一波（关卡里摆好的）和裸奔重生本来就该有，但实际看不到。
离线扫 uasset 发现：6 个车蓝图都挂了组件，可只有 BP_car_base 序列化了
ScaleInOnSpawn 这个覆盖，另外 5 个用 C++ 默认值。字符串扫描看不出那个
覆盖的**值**——如果是 false，就成了"BP_car_base 这一类车（连同它克隆出的
后续波次）从来不淡入，另外 5 类一直淡入"，很容易被误读成波次问题。

所以这里读**关卡实例**上的实际值：实例值 = 蓝图模板 + 实例覆盖，
正是运行时真正生效的那一份，比读模板更贴近现场。

只读。结果写到 Saved/check_car_scale_in.txt。
用法：py check_car_scale_in.py
"""

import traceback

import unreal

# 要报告的属性。UE 的 Python 绑定会把 bool 的前导 b 去掉：
# bScaleInOnSpawn -> scale_in_on_spawn。
PROPS = [
    "scale_in_on_spawn",
    "scale_in_duration",
    "scale_in_start_ratio",
    "scale_out_on_route_lost",
    "scale_out_duration",
    "spawn_extra_waves",
    "second_wave_delay",
    "third_wave_delay",
]

OUT = unreal.Paths.project_saved_dir() + "check_car_scale_in.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[scalein] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def read_props(comp):
    out = []
    for name in PROPS:
        try:
            out.append(u"%s=%s" % (name, comp.get_editor_property(name)))
        except Exception as exc:
            # 属性名猜错了会走到这里。报出来而不是静默跳过——
            # 静默跳过会让"读不到"长得和"值正常"一模一样。
            out.append(u"%s=读不到(%s)" % (name, type(exc).__name__))
    return u"  ".join(out)


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 车辆缩放淡入设置 ===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    comp_class = getattr(unreal, "DeliveryTrafficCarComponent", None)
    if comp_class is None:
        w(u"！unreal.DeliveryTrafficCarComponent 不存在：编辑器加载的是旧的 C++ 模块，")
        w(u"  先编译 DeliveryEditor 再重启编辑器。")
        return

    groups = {}
    for actor in eas.get_all_level_actors():
        comp = actor.get_component_by_class(comp_class)
        if comp is None:
            continue
        cls = actor.get_class().get_name()
        key = u"%s :: %s" % (cls, read_props(comp))
        groups.setdefault(key, []).append(actor.get_actor_label())

    if not groups:
        w(u"这张图里没有挂了 DeliveryTrafficCarComponent 的 actor。")
        return

    w(u"共 %d 辆车，按「车型 + 设置」分成 %d 组："
      % (sum(len(v) for v in groups.values()), len(groups)))
    for key, labels in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        w(u"")
        w(u"[%d 辆] %s" % (len(labels), key))
        w(u"    %s%s" % (u"、".join(sorted(labels)[:6]),
                         u" …" if len(labels) > 6 else u""))

    w(u"")
    w(u"怎么读这份报告：")
    w(u"  · scale_in_on_spawn=False 的那组，第一波和重生都不会淡入，")
    w(u"    而且它克隆出来的第二三波同样不会——克隆体带着同一份设置。")
    w(u"  · scale_in_duration=0 等价于关掉（代码里的判据是 >0）。")
    w(u"  · 如果所有组都是 True 且时长正常，那就不是设置问题，")
    w(u"    下一步去查 PIE 开局那一两帧的 DeltaTime——0.5 秒的淡入可能被开局卡顿吃掉了。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[scalein] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[scalein] 写入 %s" % OUT)
