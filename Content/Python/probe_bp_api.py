# -*- coding: utf-8 -*-
"""摸清这台机器上能用哪套蓝图图表 API。

上一轮发现 UBlueprint 的 function_graphs / ubergraph_pages 取不到，
但 bp 上有 add_event_dispatcher / list_member_variable_names / call_method
这些**非原生**方法，说明装了扩展蓝图 Python API 的插件。
这个脚本只做侦察不做事：
  1. 打出取图属性时的**真实报错**（是"没这属性"还是"不让读"，处理方式不同）
  2. 试着用子对象路径直接 load 出那张图（图是 UBlueprint 的子对象，
     路径形如 /Game/xx/BP.BP:FuncName，绕开属性访问）
  3. 枚举包里所有对象，看图到底叫什么、在哪
  4. dump 出名字带 Graph/Pin/Node/K2/Blueprint 的类和它们的方法

用法：py probe_bp_api.py
"""

import traceback

import unreal

BP_PATH = "/Game/Blueprint/BP_car_base"
FUNC = "TraceForIntersection"

OUT = unreal.Paths.project_saved_dir() + "bp_api.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def run():
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    w("蓝图 %s  ->  %s" % (BP_PATH, bp))
    w("")

    # --- 1. 取图属性的真实报错 ---
    w("=" * 72)
    w("1. 直接取图属性，看真实报错")
    w("=" * 72)
    for prop in ("function_graphs", "ubergraph_pages", "macro_graphs",
                 "event_graphs", "delegate_signature_graphs",
                 "FunctionGraphs", "UbergraphPages"):
        try:
            v = bp.get_editor_property(prop)
            w("  %-26s OK  %s" % (prop, type(v)))
            try:
                w("       长度 %d" % len(v))
                for g in v:
                    w("       - %s (%s)" % (g.get_name(), g.get_class().get_name()))
            except Exception as exc:
                w("       遍历失败 %s" % str(exc)[:80])
        except Exception as exc:
            w("  %-26s 失败: %s" % (prop, str(exc)[:110]))
    w("")

    # --- 2. 子对象路径直接 load ---
    w("=" * 72)
    w("2. 按子对象路径直接 load 那张图")
    w("=" * 72)
    short = BP_PATH.split("/")[-1]
    cands = [
        "%s.%s:%s" % (BP_PATH, short, FUNC),
        "%s.%s" % (BP_PATH, FUNC),
        "%s:%s" % (BP_PATH, FUNC),
        "%s.%s.%s" % (BP_PATH, short, FUNC),
    ]
    graph = None
    for c in cands:
        try:
            o = unreal.load_object(None, c)
        except Exception as exc:
            w("  %-62s 异常 %s" % (c, str(exc)[:50]))
            continue
        w("  %-62s -> %s" % (c, o))
        if o is not None and graph is None:
            graph = o
    w("")

    # --- 3. 枚举包里所有对象 ---
    w("=" * 72)
    w("3. 蓝图包里都有些什么对象（图应该在里面）")
    w("=" * 72)
    try:
        pkg = bp.get_outermost()
        objs = []
        for o in unreal.find_objects_of_class(unreal.Object) \
                if hasattr(unreal, "find_objects_of_class") else []:
            objs.append(o)
        if not objs:
            # 没有那个函数就走 EditorAssetLibrary 的资产列举
            w("  （没有 find_objects_of_class，改用逐个探测）")
        w("  包 = %s" % pkg.get_path_name())
    except Exception as exc:
        w("  失败 %s" % str(exc)[:110])
    # 用 GC 式遍历：拿所有 EdGraph 实例，筛 outer 是这个蓝图的
    for finder in ("get_objects_of_class", "find_objects_of_class",
                   "gather_objects_of_class"):
        fn = getattr(unreal, finder, None)
        if fn is None:
            continue
        w("  用 unreal.%s 找 EdGraph:" % finder)
        try:
            cls = getattr(unreal, "EdGraph", None)
            if cls is None:
                w("     unreal.EdGraph 这个类不存在")
                break
            for o in fn(cls):
                try:
                    if bp.get_name() in o.get_path_name():
                        w("     %s   (%s)" % (o.get_name(), o.get_path_name()))
                except Exception:
                    pass
        except Exception as exc:
            w("     失败 %s" % str(exc)[:90])
        break
    w("")

    # --- 4. 如果拿到图了，试着读 nodes ---
    if graph is not None:
        w("=" * 72)
        w("4. 拿到图了，试读 nodes")
        w("=" * 72)
        w("  图类 = %s" % graph.get_class().get_name())
        for prop in ("nodes", "Nodes"):
            try:
                nds = graph.get_editor_property(prop)
                w("  %s OK，共 %d 个节点" % (prop, len(nds)))
                for nd in nds[:8]:
                    w("     %s" % nd.get_class().get_name())
                break
            except Exception as exc:
                w("  %s 失败: %s" % (prop, str(exc)[:110]))
        w("  图的可用成员：")
        for n in sorted(set(dir(graph))):
            if not n.startswith("_"):
                w("     %s" % n)
        w("")

    # --- 5. API 清单 ---
    w("=" * 72)
    w("5. 名字带 Graph/Pin/Node/K2 的类")
    w("=" * 72)
    names = sorted(n for n in dir(unreal)
                   if any(k in n for k in ("EdGraph", "GraphPin", "GraphNode",
                                           "K2Node", "BlueprintEditor",
                                           "BlueprintGraph")))
    for n in names:
        w("  unreal.%s" % n)
        obj = getattr(unreal, n, None)
        if obj is None:
            continue
        mems = [m for m in sorted(set(dir(obj)))
                if not m.startswith("_") and m not in (
                    "cast", "get_class", "get_fname", "get_full_name",
                    "get_name", "get_outer", "get_outermost", "get_package",
                    "get_path_name", "get_typed_outer", "get_world",
                    "static_class", "modify", "rename",
                    "get_editor_property", "set_editor_property",
                    "set_editor_properties", "reset_editor_property",
                    "is_editor_property_overridden",
                    "acquire_editor_element_handle", "call_method")]
        for m in mems:
            w("       %s" % m)
    w("")
    w("  （上面过滤掉了每个 UObject 都有的通用方法，只留特有的）")

    # --- 6. bp.call_method 能不能直接调 C++ 侧 ---
    w("")
    w("=" * 72)
    w("6. 扩展方法的签名")
    w("=" * 72)
    for m in ("call_method", "list_member_variable_names", "add_event_dispatcher",
              "list_event_dispatchers"):
        fn = getattr(bp, m, None)
        w("  %s -> %s" % (m, getattr(fn, "__doc__", None)))

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[bpapi] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[bpapi] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
