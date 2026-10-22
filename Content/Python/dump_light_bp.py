# -*- coding: utf-8 -*-
"""把红绿灯蓝图 /Game/trafficlight/trafficlight 整个导出来：图、变量默认值、组件。

为什么要导
----------
离线扫 uasset 的名字表发现这个蓝图**自带一整套变色循环**：
ReceiveBeginPlay + Delay + 自定义事件 "traffic light color"、
三个函数 "toggle green/yellow/red light"、变量 LightColor / Start Color /
Green|Yellow|Red Light Duration、三个灯组件 + SpotLightComponent。

如果它真的自己会循环，那新加的 C++ 组件是多余的、还会和它抢着写 LightColor
（两个独立定时器各写一次，表现是乱闪或者卡在一个色上）。
但名字表只能看出"有这些名字"，看不出连线——
按 ASCII 扫 uasset 推断结论在本项目已经错过一次（材质槽位那回）。
所以这里导真实的图和真实的默认值再做决定。

另外图里的节点标题和引脚名会给出变量的**准确 FName**。
名字表里同时有 "Green Light Duration" 和 "green light duration"
（一个像显示名、一个像真名），靠猜必然踩空，反射读属性读不到就是静默返回空。

只读，不改图、不改值。
用法：py dump_light_bp.py
"""

import traceback

import unreal

BP_PATH = "/Game/trafficlight/trafficlight"

# 想读的变量。每个给几种拼法——UE 的蓝图变量名可以带空格，
# 而 Python 绑定对原生属性是 snake_case、对蓝图变量要用原名，
# 三种都试一遍比猜一种然后"读不到"有用。
VAR_GUESSES = [
    ("当前颜色",   ["LightColor", "light color", "Light Color"]),
    ("起始颜色",   ["Start Color", "StartColor", "start color", "start_color"]),
    ("绿灯时长",   ["Green Light Duration", "GreenLightDuration",
                    "green light duration"]),
    ("黄灯时长",   ["Yellow Light Duration", "YellowLightDuration",
                    "yellow light duration"]),
    ("红灯时长",   ["Red Light Duration", "RedLightDuration",
                    "red light duration"]),
    ("组号",       ["LightNumber", "light number", "Light Number"]),
    ("路口周期",   ["LightDuration", "light duration"]),
]

OUT = unreal.Paths.project_saved_dir() + "dump_light_bp.txt"
lines = []

BEL = unreal.BlueprintEditorLibrary
BGE = getattr(unreal, "BlueprintGraphEditor", None)


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def attempt(label, *calls):
    last = u"（没有可试的调用）"
    for i, c in enumerate(calls):
        try:
            return c(), None
        except Exception as exc:
            last = u"形状%d: %s" % (i + 1, str(exc)[:110])
    return None, u"%s 全部失败  %s" % (label, last)


def pin_str(p):
    # get_pin_name() 返回 unreal.Name 不是 str，直接切片会抛
    # "'Name' object is not subscriptable"，整个导出断在第一个节点。
    def g(fn, dflt=u"?"):
        try:
            v = fn()
            return dflt if v is None else str(v)
        except Exception:
            return dflt
    return (g(p.get_pin_name), g(p.get_pin_direction).split(".")[-1],
            g(p.get_pin_type_display_string), g(p.get_pin_value, u""))


def node_label(nd):
    if nd is None:
        return u"<空>"
    for how in (lambda: nd.get_node_title(), lambda: BEL.get_node_title(nd)):
        try:
            t = how()
            if t:
                return str(t).replace("\n", " / ")
        except Exception:
            pass
    try:
        return nd.get_class().get_name()
    except Exception:
        return u"?"


def dump_vars(bp):
    w(u"=" * 78)
    w(u"变量默认值（读 CDO）")
    w(u"=" * 78)
    gen = bp.generated_class()
    if gen is None:
        w(u"！generated_class() 是 None —— 蓝图没编译，或者编辑器加载的 C++ 模块")
        w(u"  和资产对不上（之前遇到过加载了过期 dylib 的情况）。")
        return
    cdo = unreal.get_default_object(gen)
    if cdo is None:
        w(u"！拿不到 CDO。")
        return
    for human, names in VAR_GUESSES:
        got = False
        for n in names:
            try:
                val = cdo.get_editor_property(n)
            except Exception:
                continue
            w(u"  %-10s %-24s = %s" % (human, n, val))
            got = True
            break
        if not got:
            w(u"  %-10s %-24s   这些拼法都读不到：%s"
              % (human, u"", u" / ".join(names)))


def dump_components(bp):
    w(u"")
    w(u"=" * 78)
    w(u"组件树")
    w(u"=" * 78)
    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles, err = attempt(u"gather_subobject_data_for_blueprint",
                           lambda: sds.k2_gather_subobject_data_for_blueprint(bp))
    if err:
        w(u"  " + err)
        return
    for h in handles:
        data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(h)
        try:
            obj = unreal.SubobjectDataBlueprintFunctionLibrary \
                .get_object_for_blueprint(data, bp)
            cls = obj.get_class().get_name() if obj else u"?"
        except Exception:
            cls = u"?"
        try:
            nm = str(unreal.SubobjectDataBlueprintFunctionLibrary
                     .get_variable_name(data))
        except Exception:
            nm = u"?"
        w(u"  %-30s %s" % (nm, cls))


def dump_graphs(bp):
    names, err = attempt(u"list_graph_names",
                         lambda: BEL.list_graph_names(bp),
                         lambda: BEL.list_graph_names(bp, True))
    w(u"")
    w(u"=" * 78)
    w(u"图清单")
    w(u"=" * 78)
    if err:
        w(u"  " + err)
        return
    for n in names:
        w(u"  %s" % n)
    flush()

    for gname in names:
        w(u"")
        w(u"=" * 78)
        w(u"图 %s" % gname)
        w(u"=" * 78)
        ge, err = attempt(u"取 graph editor",
                          lambda: BGE.get_graph_editor_by_name(bp, gname),
                          lambda: BGE.get_graph_editor_by_name(bp, str(gname)),
                          lambda: BGE.get_graph_editor(BEL.find_graph(bp, gname)))
        if err:
            w(u"  " + err)
            flush()
            continue
        nodes, err = attempt(u"list_all_nodes", lambda: ge.list_all_nodes())
        if err:
            w(u"  " + err)
            flush()
            continue
        w(u"  节点 %d 个" % len(nodes))

        rows = []
        for nd in nodes:
            try:
                pos = nd.get_node_pos()
                x, y = float(pos[0]), float(pos[1])
            except Exception:
                x, y = 0.0, 0.0
            rows.append((x, y, nd))
        rows.sort(key=lambda r: (r[0], r[1]))   # 按画布 X ≈ 执行顺序

        for x, y, nd in rows:
            w(u"-" * 78)
            w(u"[%6.0f,%6.0f] %s" % (x, y, node_label(nd)))
            pins, perr = attempt(u"pins",
                                 lambda: nd.list_all_pins(),
                                 lambda: BEL.list_all_pins(nd))
            if perr:
                w(u"            " + perr)
                continue
            for p in pins:
                nm, d, ty, v = pin_str(p)
                head = u"    %-4s %-26s %-20s" % (d[:4], nm[:26], ty[:20])
                if v and v != u"None":
                    head += u" = %s" % v[:40]
                w(head)
                try:
                    conns = p.list_connected_pins()
                except Exception:
                    conns = []
                for c in conns:
                    try:
                        w(u"          -> %s" % node_label(c.get_owning_node()))
                    except Exception:
                        pass
        flush()


def run():
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    w(u"=== 红绿灯蓝图导出 ===")
    w(u"资产：%s" % BP_PATH)
    if bp is None:
        w(u"！加载不到这个资产。")
        return
    dump_vars(bp)
    dump_components(bp)
    dump_graphs(bp)
    w(u"")
    w(u"导出完成，全程只读。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[lightbp] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[lightbp] 写入 %s" % OUT)
