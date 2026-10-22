# -*- coding: utf-8 -*-
"""建一个只改默认值的红绿灯子类，用来让两组灯错开相位。

为什么要绕这一圈
----------------
两组灯互为红绿，靠的是各自不同的 `start color`。但那个蓝图变量**没开
Instance Editable**（带 `CPF_DisableEditOnInstance`），于是：

    PropertyAccessUtil.cpp:739   不是 CDO 的对象 + 该标记 -> PermissionDenied

所以在关卡实例上写它必定失败。而打开那个开关只能在蓝图界面点，
`UBlueprint::NewVariables` 是裸 `UPROPERTY()`（没有 BlueprintReadWrite /
EditAnywhere），Python 读不到也写不了。

但同一个检查里，**写 CDO（模板）是允许的**——挡模板要的是另一个标记
`CPF_DisableEditOnTemplate`，这个变量没有（否则它在 Class Defaults 里也改不了）。
于是换个思路：不在实例上存差异，让**类**来承载差异。
子类只改一个默认值、一个节点都没有，逻辑全继承父类，没有两份逻辑要同步。

分工
----
    父类 trafficlight              start color = 红   -> 给"后放行"那组
    子类 trafficlight_green_start  start color = 绿   -> 给"先放行"那组

三个时长写在**两个 CDO** 上，这样新摆的灯默认就是对的、不依赖实例覆盖
（时长本来就能写实例，但能少依赖一处就少一处）。

幂等：子类已存在就复用，不会重建。
用法：py make_light_variant.py     跑完接着跑 gen_traffic_lights.py
"""

import traceback

import unreal

PARENT_BP = "/Game/trafficlight/trafficlight"
VARIANT_PATH = "/Game/trafficlight"
VARIANT_NAME = "trafficlight_green_start"

INTERSECTION_PREFIX = "BP_Intersection"
DURATION_PROP = "LightDuration"
YELLOW = 1.0
GREEN, RED = 0, 2        # trafficlight_color_enum: 0=green 1=yellow 2=red
ENUM_TYPE = "trafficlight_color_enum"

# 蓝图变量真名带空格，是真名不是显示名（dump_light_bp.py 读 CDO 确认过）。
# 这几行曾被我自己的补丁切掉过一次：改动用"从 A 到 B 整段替换"的方式，
# 而它们正好夹在中间，脚本直接 NameError 崩掉、一个值都没写。
P_START, P_GREEN, P_YELLOW, P_RED = (
    "Start Color", "Green Light Duration", "Yellow Light Duration",
    "Red Light Duration")


def enum_like(sample, i, name):
    """把整数换成枚举值对象，**类型从读到的现值反推**。

    写裸整数会被拒：
        Failed to convert type 'int' to property 'start color' (ByteProperty)
    这个变量是 UserDefinedEnum 撑的 ByteProperty，只认枚举值对象。
    （和早先在**实例**上被拒是两回事：那次是权限 CPF_DisableEditOnInstance，
    这里权限已经过了，纯粹是类型。）

    为什么从 sample 反推而不是 getattr(unreal, "trafficlight_color_enum")：
    后者实测取不到（蓝图里定义的 UserDefinedEnum 不作为模块属性暴露），
    三种取法全落空、静默退回整数，于是报错和改之前一模一样、看着像没生效。
    而回读那个属性拿到的就是枚举值对象（repr 是
    `<trafficlight_color_enum.RED: 2>`），它的 type 就是我们要的枚举类型。

    返回 (值, 用了哪种取法)。取不到就返回 (整数, 说明)，
    让引擎再报一次原错——别自己编一个值写进去。
    """
    et = type(sample) if sample is not None else None
    cands = []
    if et is not None:
        cands += [(u"type(现值).%s" % name, lambda: getattr(et, name)),
                  (u"type(现值)(%d)" % i, lambda: et(i)),
                  (u"type(现值).cast", lambda: et.cast(i))]
        # 成员遍历：上面几种都不行时，按 value 找那个成员
        def by_value():
            for m in et:
                if int(getattr(m, "value", m)) == i:
                    return m
            return None
        cands.append((u"遍历成员找 value==%d" % i, by_value))
    mod = getattr(unreal, ENUM_TYPE, None)
    if mod is not None:
        cands.append((u"unreal.%s.%s" % (ENUM_TYPE, name),
                      lambda: getattr(mod, name)))
    for how, fn in cands:
        try:
            v = fn()
            if v is not None:
                return v, how
        except Exception:
            pass
    return i, u"全部取法失败，退回整数（现值类型 %s）" % (
        et.__name__ if et is not None else u"未知")


OUT = unreal.Paths.project_saved_dir() + "make_light_variant.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[lightvar] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def read_cycle():
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


def write_cdo(bp, start_int, start_name, green, yellow, red, who):
    """往 CDO 写默认值。写模板是被允许的，写实例才被挡。"""
    gen = bp.generated_class()
    if gen is None:
        w(u"  ！%s 的 generated_class() 是 None（没编译？）" % who)
        return False
    cdo = unreal.get_default_object(gen)
    if cdo is None:
        w(u"  ！%s 取不到 CDO" % who)
        return False

    # 先读现值，拿它的类型来构造要写的枚举值
    try:
        sample = cdo.get_editor_property(P_START)
    except Exception as exc:
        w(u"  ！%s 连读 %s 都失败：%s" % (who, P_START, str(exc)[:90]))
        sample = None
    start_val, how = enum_like(sample, start_int, start_name)
    w(u"  %s：枚举值取法 = %s" % (who, how))

    ok = True
    for name, val in ((P_START, start_val), (P_GREEN, green),
                      (P_YELLOW, yellow), (P_RED, red)):
        try:
            cdo.set_editor_property(name, val)
        except Exception as exc:
            w(u"  ！%s 写 %s 失败：%s" % (who, name, str(exc)[:110]))
            ok = False

    # 不管有没有写失败都编译+存盘：失败也可能只是某一个属性，
    # 其余值该留住；而且不存盘的话资产只活在内存里，
    # does_asset_exist 看得见、磁盘上没有，排查时非常误导。
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    # 回读。"写了"不等于"存住了"，这条在本项目栽过。
    back = []
    for name in (P_START, P_GREEN, P_YELLOW, P_RED):
        try:
            back.append(u"%s=%s" % (name.split()[0],
                                    cdo.get_editor_property(name)))
        except Exception:
            back.append(u"%s=读不到" % name.split()[0])
    w(u"  %-28s %s" % (who, u"  ".join(back)))
    return ok


def run():
    w(u"=== 红绿灯数据子类 ===")
    parent = unreal.EditorAssetLibrary.load_asset(PARENT_BP)
    if parent is None:
        w(u"！加载不到父类 %s" % PARENT_BP)
        return
    pgen = parent.generated_class()
    if pgen is None:
        w(u"！父类没编译，generated_class() 是 None。先在编辑器里编译它。")
        return

    cycle, inter_label = read_cycle()
    if cycle is None:
        w(u"！读不到路口的 %s —— 没有对齐基准，不写任何东西。" % DURATION_PROP)
        return
    green = cycle - YELLOW
    if green <= 0.0:
        w(u"！路口周期 %.2f 比黄灯 %.2f 还短。先把 YELLOW 调小。" % (cycle, YELLOW))
        return
    w(u"路口 %s 的 %s = %.2f 秒 → 一圈 %.2f 秒（绿 %.2f + 黄 %.2f + 红 %.2f）"
      % (inter_label, DURATION_PROP, cycle, cycle * 2, green, YELLOW, cycle))
    w(u"")

    full = u"%s/%s" % (VARIANT_PATH, VARIANT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        variant = unreal.EditorAssetLibrary.load_asset(full)
        w(u"子类已存在，复用：%s" % full)
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", pgen)
        variant = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            VARIANT_NAME, VARIANT_PATH, None, factory)
        if variant is None:
            w(u"！建子类失败：%s" % full)
            return
        w(u"已建子类：%s（父类 %s）" % (full, pgen.get_name()))
    w(u"")

    w(u"写 CDO 默认值（回读确认）：")
    write_cdo(parent, RED, u"RED", green, YELLOW, cycle,
              u"父类（后放行组用）")
    write_cdo(variant, GREEN, u"GREEN", green, YELLOW, cycle,
              u"子类（先放行组用）")

    w(u"")
    w(u"下一步：")
    w(u"  · 关卡里还没摆灯，或者改过 GREEN_FIRST_GROUP → 跑 py gen_traffic_lights.py")
    w(u"    （它按组号给「先放行」那组用子类、另一组用父类，先删后摆 4 盏）")
    w(u"  · 灯已经摆好、只是改了上面这些默认值 → **不用重摆**。")
    w(u"    灯没有实例覆盖，起始色和时长都跟着各自类的默认值走，改完即生效。")
    w(u"  两种情况都要 Ctrl+S 保存关卡。")
    w(u"")
    w(u"验收只看一条：灯是绿的时候，这个方向的车在走。")
    w(u"反了就把 gen_traffic_lights.py 的 GREEN_FIRST_GROUP 翻一下，")
    w(u"**两个脚本的这个常量必须一致**，然后两条都重跑。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[lightvar] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[lightvar] 写入 %s" % OUT)
