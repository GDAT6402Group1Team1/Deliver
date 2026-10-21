# -*- coding: utf-8 -*-
"""让关卡里的红绿灯模型和路口的灯态同步——**只改数据，不加任何组件**。

为什么不需要 C++
----------------
`trafficlight` 蓝图本来就自带完整的变色循环（dump_light_bp.py 导出来确认过）：

    BeginPlay -> set traffic light color(start color)
    set traffic light color(c): Set「trafficlight colors」= c
      Switch(c): 0绿 -> 绿亮 -> Delay(green light duration) -> 自调用(1黄)
                 1黄 -> 黄亮 -> Delay(yellow light duration) -> 自调用(2红)
                 2红 -> 红亮 -> Delay(red light duration)   -> 自调用(0绿)

亮灯靠的是 `toggle X light` 改 SpotLight 的可见性，只在那个自定义事件里发生。
所以**从外部写变量是没用的**（组件化的做法试过，走不通，已撤掉）：
要让它和路口同步，只能让它自己的节拍和路口一致。

两件事就够了
------------
1. **一圈时长 = 路口周期 × 2**。路口是两相位轮流放行（本图 LightDuration=6），
   所以单盏灯的视角是「绿 6 秒、红 6 秒」，一圈 12 秒。
   黄灯从绿的那段里切出来（绿 5 + 黄 1 + 红 6），保持总长不变——
   黄灯额外加时间的话一圈就变 13 秒，和路口差 1 秒，每圈都在漂。
   原始值是绿 10 / 黄 3 / 红 20 = 33 秒，和路口 6 秒毫无关系，这就是不同步的根源。
2. **两组灯的 Start Color 相反**：一组从绿开始、另一组从红开始，
   自然错开一个相位。组号来自 gen_traffic_lights.py 打的标签
   「ClaudeGenTrafficLight:<n>」，和路口段的 LightNumber 是同一套。

已知的局限，先写在这儿别以后当 bug 查
------------------------------------
灯和路口是**两个独立的定时器**，都在 BeginPlay 起跑。蓝图 Delay 是
「到了第一个 elapsed >= duration 的帧才继续」，每次都会多走不到一帧，
所以灯相对路口会**慢慢往后漂**（60fps 下一圈约 25ms，一小时量级才到秒）。
几分钟的试玩看不出来；真要彻底不漂，就得让灯读路口的相位，
那就不是"纯时间"了——这是当时选纯时间时接受的代价。

用法：py tune_traffic_lights.py
"""

import traceback

import unreal

TAG = "ClaudeGenTrafficLight"
INTERSECTION_PREFIX = "BP_Intersection"
DURATION_PROP = "LightDuration"          # 路口上那个周期变量

# 黄灯时长。从绿灯那段里切出来，不额外占时间。
YELLOW = 1.0

# 2026-10-21 用户定义：上下(X/组0)绿起始，左右(Y/组1)红起始。
GREEN_FIRST_GROUP = 0

# 枚举值：0=green / 1=yellow / 2=red（查 trafficlight_color_enum.uasset 的名字表确认）
GREEN, RED = 0, 2

# 蓝图变量名带空格，是真名不是显示名——dump 出来的 CDO 就是这么读到的
P_START, P_GREEN, P_YELLOW, P_RED = (
    "Start Color", "Green Light Duration", "Yellow Light Duration",
    "Red Light Duration")

OUT = unreal.Paths.project_saved_dir() + "tune_traffic_lights.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[tunelight] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def group_of(actor):
    """从标签「TAG:<n>」取组号；没有就返回 None（而不是默认 0——
    默认 0 会把"标签丢了"伪装成"属于第 0 组"，两组灯同时变绿看不出原因）。"""
    for t in actor.tags:
        s = str(t)
        if s.startswith(TAG + ":"):
            suffix = s[len(TAG) + 1:]
            if suffix.isdigit():
                return int(suffix)
    return None


def read_cycle():
    """路口的周期。读不到就返回 None，不编一个默认值。"""
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for a in eas.get_all_level_actors():
        if not a.get_class().get_name().startswith(INTERSECTION_PREFIX):
            continue
        try:
            v = float(a.get_editor_property(DURATION_PROP))
            if v > 0.0:
                return v, a.get_actor_label()
        except Exception:
            pass
    return None, None


def enum_val(i):
    """整数 -> trafficlight_color_enum 的枚举值对象。

    start color 是 UserDefinedEnum 撑的 ByteProperty，写裸整数会被拒：
        Failed to convert type 'int' to property 'start color' (ByteProperty)
    """
    et = getattr(unreal, "trafficlight_color_enum", None)
    if et is None:
        return i
    for how in (lambda: et(i), lambda: et.cast(i)):
        try:
            v = how()
            if v is not None:
                return v
        except Exception:
            pass
    return i


def setp(actor, name, value):
    try:
        actor.set_editor_property(name, value)
        return True, u""
    except Exception as exc:
        return False, u"%s: %s" % (name, str(exc)[:90])


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 红绿灯节拍对齐 ===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    cycle, inter_label = read_cycle()
    if cycle is None:
        w(u"！读不到路口的 %s —— 没有对齐的基准，不写任何东西。" % DURATION_PROP)
        w(u"  这张图里有 %s 开头的 actor 吗？没有就说明灯和路口不在同一关卡。"
          % INTERSECTION_PREFIX)
        return
    green = cycle - YELLOW
    if green <= 0.0:
        w(u"！路口周期 %.2f 秒比黄灯 %.2f 秒还短，绿灯会变成负数。先把 YELLOW 调小。"
          % (cycle, YELLOW))
        return
    w(u"路口 %s 的 %s = %.2f 秒（两相位，所以灯一圈 %.2f 秒）"
      % (inter_label, DURATION_PROP, cycle, cycle * 2))
    w(u"要写入的时长：绿 %.2f + 黄 %.2f + 红 %.2f = %.2f 秒"
      % (green, YELLOW, cycle, green + YELLOW + cycle))
    w(u"第 %d 组从绿开始，另一组从红开始。" % GREEN_FIRST_GROUP)
    w(u"")

    lights = [a for a in eas.get_all_level_actors()
              if any(str(t) == TAG or str(t).startswith(TAG + ":") for t in a.tags)]
    if not lights:
        w(u"！这张图里没有带 %s 标签的灯。" % TAG)
        w(u"  先跑 gen_traffic_lights.py 摆灯；手动摆的灯没有标签，本脚本认不出来。")
        return

    w(u"找到 %d 盏灯：" % len(lights))
    ok = 0
    for a in sorted(lights, key=lambda x: x.get_actor_label()):
        label = a.get_actor_label()
        g = group_of(a)
        if g is None:
            w(u"  %-34s 标签里没有组号，跳过（标签：%s）"
              % (label, u",".join(str(t) for t in a.tags)))
            continue
        start = GREEN if (g % 2) == (GREEN_FIRST_GROUP % 2) else RED

        # 改之前必须 modify(True)：普通的 set_editor_property 虽然会标脏，
        # 但实例覆盖这种东西按 CLAUDE.md 那条铁律统一先 modify，不赌引擎行为。
        a.modify(True)
        errs = []
        for name, val in ((P_START, enum_val(start)), (P_GREEN, green),
                          (P_YELLOW, YELLOW), (P_RED, cycle)):
            good, err = setp(a, name, val)
            if not good:
                errs.append(err)
        want = u"绿" if start == GREEN else u"红"
        if errs:
            # start color 写实例一定会被挡（变量没开 Instance Editable），
            # 所以改由类承载：只要这盏灯**当前的值**已经等于期望值，就算过。
            # 不这样分的话，走子类方案时每次都报"写失败"、把成功的运行
            # 看成失败的，下次又会有人去追一个已经解决的问题。
            cur = None
            try:
                raw = a.get_editor_property(P_START)
                # 读回来是枚举值对象，不是 int——int() 会抛，
                # 抛了就走到 cur=None、把"值其实是对的"报成"不符"。
                cur = int(getattr(raw, "value", raw))
            except Exception:
                cur = None
            if cur == start:
                ok += 1
                w(u"  %-34s 组%d  起始%s（由类默认值提供，实例写入被挡是正常的）"
                  % (label, g, want))
                continue
            w(u"  %-34s 组%d  应为起始%s 实际%s  **不符**：%s"
              % (label, g, want,
                 u"?" if cur is None else (u"绿" if cur == GREEN else u"红"),
                 u"；".join(errs)))
            w(u"      这盏灯的类是 %s —— 起始色由类决定，"
              % a.get_class().get_name())
            w(u"      所以要么它用错了类（重跑 gen_traffic_lights.py），")
            w(u"      要么子类还没建（先跑 make_light_variant.py）。")
            continue
        ok += 1
        w(u"  %-34s 组%d  起始%s" % (label, g, want))

    # 回读验证。"写了 N 个"只能证明函数被调用过，证明不了值留住了。
    w(u"")
    w(u"回读验证：")
    for a in sorted(lights, key=lambda x: x.get_actor_label()):
        vals = []
        for name in (P_START, P_GREEN, P_YELLOW, P_RED):
            try:
                vals.append(u"%s=%s" % (name.split()[0], a.get_editor_property(name)))
            except Exception:
                vals.append(u"%s=读不到" % name.split()[0])
        w(u"  %-34s %-34s %s"
          % (a.get_actor_label(), a.get_class().get_name(), u"  ".join(vals)))

    w(u"")
    w(u"写好 %d/%d 盏。**记得 Ctrl+S 保存关卡**，脚本不替你存。" % (ok, len(lights)))
    w(u"进 PIE 对照着看两件事：")
    w(u"  1. 灯真的在变色（绿 %.0f 秒 → 黄 %.0f 秒 → 红 %.0f 秒 循环）" % (green, YELLOW, cycle))
    w(u"  2. 灯是绿的时候，这个方向的车确实在走。反了就把本文件顶部")
    w(u"     GREEN_FIRST_GROUP 从 %d 改成 %d 再跑一次。"
      % (GREEN_FIRST_GROUP, 1 - GREEN_FIRST_GROUP))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[tunelight] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[tunelight] 写入 %s" % OUT)
