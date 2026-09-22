# -*- coding: utf-8 -*-
"""把 BP_car_base 的函数图连同连线导出成文本。

第一版走 UBlueprint 的图属性，失败了（FunctionGraphs/Nodes 都是 protected）。
侦察后发现这台机器上有完整的蓝图图表 API：
    unreal.BlueprintEditorLibrary   list_graphs / find_graph / get_node_title
                                    list_all_pins / list_input_pins / get_node_pos
    unreal.BlueprintGraphEditor     get_graph_editor_by_name / list_all_nodes
    unreal.BlueprintGraphPin        get_pin_name / get_pin_value / list_connected_pins
                                    get_owning_node / get_pin_direction
连线就是靠 pin.list_connected_pins() -> 对端 pin.get_owning_node() 还原的。

签名未知，所以每个关键函数先打 __doc__ 再试几种调用形状——
就算全挂了，输出里也留下了签名，下一轮能直接对着写。

只读，不改图。
用法：py dump_bp_graph.py
"""

import traceback

import unreal

# (蓝图路径, 要导的图名列表)；图名列表为空 = 该蓝图的全部图
TARGETS = [
    # 目标：TraceForIntersection 现在查的是 Traffic_Road 通道，
    # Lane_ 和 Inter_ 的 Box 都在这个通道上，分不出普通车道和路口段。
    # 要改成只认 IntersectionChild，先得看清它现在的节点和连线。
    # 探测球体在路口总是朝车的正右方：终点 = 未来点 + GetForwardVector(Rotation)*200，
    # 方向完全由 GetFuturePostionandRotationAlongSpline 返回的 Rotation 决定，
    # 那是个自定义函数，先把它导出来看。
    ("/Game/Blueprint/BP_car_base", ["GetFuturePostionandRotationAlongSpline",
                                     "TraceForIntersection",
                                     "TraceForNewPath"]),
]

OUT = unreal.Paths.project_saved_dir() + "bp_graph.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def doc(obj, name):
    fn = getattr(obj, name, None)
    if fn is None:
        return "  %-30s （没有这个方法）" % name
    d = (getattr(fn, "__doc__", "") or "").strip().replace("\n", " ")
    return "  %-30s %s" % (name, d[:150])


def attempt(label, *calls):
    """挨个试几种调用形状，返回第一个成功的结果。"""
    for i, c in enumerate(calls):
        try:
            return c(), None
        except Exception as exc:
            last = "形状%d: %s" % (i + 1, str(exc)[:100])
    return None, "%s 全部失败  %s" % (label, last)


BEL = unreal.BlueprintEditorLibrary
BGE = getattr(unreal, "BlueprintGraphEditor", None)


def pin_str(p):
    """引脚的 (名字, 方向, 类型, 值)，一律转成 str。

    get_pin_name() 返回的是 unreal.Name 不是 str，直接切片会抛
    "'Name' object is not subscriptable"，整个导出就断在这里。
    """
    def g(fn, dflt="?"):
        try:
            v = fn()
            return dflt if v is None else str(v)
        except Exception:
            return dflt

    nm = g(p.get_pin_name)
    d = g(p.get_pin_direction).split(".")[-1]
    ty = g(p.get_pin_type_display_string)
    v = g(p.get_pin_value, "")
    return nm, d, ty, ("" if v in ("None", "") else v)


def node_label(nd):
    if nd is None:
        return "<空>"
    for how in (lambda: nd.get_node_title(),
                lambda: BEL.get_node_title(nd)):
        try:
            t = how()
            if t:
                return str(t).replace("\n", " / ")
        except Exception:
            pass
    try:
        return nd.get_class().get_name()
    except Exception:
        return "?"


def run():
    first = True
    for bp_path, want in TARGETS:
        dump_one(bp_path, want, show_api=first)
        first = False
    w("导出完成，只读，没有改动任何东西。")
    flush()
    unreal.log("[bpdump] 写入 %s" % OUT)


def dump_one(BP_PATH, WANT, show_api):
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    w("")
    w("#" * 78)
    w("# 蓝图 %s" % BP_PATH)
    w("#" * 78)
    if bp is None:
        w("!! 加载不到")
        flush()
        return

    if not show_api:
        pass
    else:
        w("=" * 78)
        w("关键函数的签名（万一后面调用挂了，照着这个改）")
        w("=" * 78)
        for n in ("list_graphs", "list_graph_names", "find_graph",
                  "get_node_title", "list_all_pins", "list_input_pins",
                  "list_output_pins", "get_node_pos"):
            w(doc(BEL, n))
        if BGE is not None:
            for n in ("get_graph_editor_by_name", "get_graph_editor",
                      "list_all_nodes", "list_nodes_of_class"):
                w(doc(BGE, n))
        P = getattr(unreal, "BlueprintGraphPin", None)
        if P is not None:
            for n in ("get_pin_name", "get_pin_value", "list_connected_pins",
                      "get_owning_node", "get_pin_direction"):
                w(doc(P, n))
        w("")
    flush()

    # --- 图清单 ---
    w("=" * 78)
    w("这个蓝图有哪些图")
    w("=" * 78)
    names, err = attempt("list_graph_names",
                         lambda: BEL.list_graph_names(bp),
                         lambda: BEL.list_graph_names(bp, True))
    if err:
        w("  " + err)
    else:
        for n in names:
            w("  %s" % n)
    w("")
    flush()

    targets = [n for n in (names or []) if not WANT or str(n) in WANT]
    if not targets:
        w("!! 目标 %s 不在清单里，改用清单第一个" % WANT)
        targets = list(names or [])[:1]

    for gname in targets:
        w("=" * 78)
        w("图 %s" % gname)
        w("=" * 78)

        ge, err = attempt("取 graph editor",
                          lambda: BGE.get_graph_editor_by_name(bp, gname),
                          lambda: BGE.get_graph_editor_by_name(bp, str(gname)),
                          lambda: BGE.get_graph_editor(BEL.find_graph(bp, gname)))
        if err:
            w("  " + err)
            flush()
            continue

        nodes, err = attempt("list_all_nodes", lambda: ge.list_all_nodes())
        if err:
            w("  " + err)
            flush()
            continue
        w("  节点 %d 个" % len(nodes))
        w("")

        # 按画布 X 排序 ≈ 执行顺序
        rows = []
        for nd in nodes:
            try:
                pos = nd.get_node_pos()
                x, y = float(pos[0]), float(pos[1])
            except Exception:
                x, y = 0.0, 0.0
            rows.append((x, y, nd))
        rows.sort(key=lambda r: (r[0], r[1]))

        for x, y, nd in rows:
            w("-" * 78)
            w("[%6.0f,%6.0f] %s" % (x, y, node_label(nd)))
            try:
                w("            类 %s" % nd.get_class().get_name())
            except Exception:
                pass
            pins, perr = attempt("pins",
                                 lambda: nd.list_all_pins(),
                                 lambda: BEL.list_all_pins(nd))
            if perr:
                w("            " + perr)
                continue
            for p in pins:
                nm, d, ty, v = pin_str(p)
                head = "    %-4s %-26s %-22s" % (d[:4], nm[:26], ty[:22])
                if v:
                    head += " = %s" % v[:40]
                w(head)
                try:
                    conns = p.list_connected_pins()
                except Exception:
                    conns = []
                for cp in conns or []:
                    cnm, _cd, _cty, _cv = pin_str(cp)
                    try:
                        owner = cp.get_owning_node()
                    except Exception:
                        owner = None
                    w("             -> %s . %s" % (node_label(owner), cnm))
        w("")
        flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[bpdump] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
