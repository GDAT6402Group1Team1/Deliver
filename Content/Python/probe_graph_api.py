# -*- coding: utf-8 -*-
"""动 BP_car_base 的图之前，先把要用的 API 签名和节点名确认清楚。

要做的手术是：TraceForIntersection 里把 SphereTraceSingleForObjects 换成
多结果版 + 遍历 + 逐个 Cast，这样普通车道 Box 不会再把这一帧顶掉。
盲改引脚一旦搞错，蓝图会坏掉且不好还原，所以先只读地确认：

  1. add_call_function_node / add_macro_node / try_create_connection 的签名
  2. 多结果球形扫描那个函数在 Python 侧叫什么、属于哪个类
  3. ForEachLoopWithBreak 宏的资产路径（要 Break 才能命中一个就停）
  4. 现有节点的引脚名到底是什么（OutHits / Array / ArrayElement ...）

只读，什么都不改。
用法：py probe_graph_api.py
"""

import traceback

import unreal

BP_PATH = "/Game/Blueprint/BP_car_base"
GRAPH = "TraceForIntersection"
OUT = unreal.Paths.project_saved_dir() + "graph_api.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def sig(cls, names):
    for n in names:
        try:
            f = getattr(cls, n)
        except AttributeError:
            w("   %-34s 没有这个函数" % n)
            continue
        doc = (f.__doc__ or "").replace("\n", " ")
        w("   %-34s %s" % (n, doc[:150]))


def run():
    w("=" * 92)
    w("1. 建节点 / 连线相关的签名")
    w("=" * 92)
    w(" unreal.BlueprintGraphEditor:")
    sig(unreal.BlueprintGraphEditor,
        ["add_call_function_node", "add_macro_node", "add_branch_node",
         "add_node_pin", "remove_node_pin", "remove_nodes",
         "retarget_node_class", "create_node_from_name",
         "list_available_nodes", "list_all_nodes", "get_graph_editor_by_name"])
    w("")
    w(" unreal.BlueprintGraphPin:")
    sig(unreal.BlueprintGraphPin,
        ["try_create_connection", "can_create_connection", "break_pin_links",
         "break_single_pin_link", "get_pin_name", "list_connected_pins",
         "get_owning_node", "get_default_value", "set_default_value"])
    w("")
    flush()

    w("=" * 92)
    w("2. 多结果球形扫描叫什么（在 KismetSystemLibrary 里找）")
    w("=" * 92)
    for n in sorted(dir(unreal.SystemLibrary)):
        if "sphere_trace" in n or "trace_multi" in n:
            w("   unreal.SystemLibrary.%s" % n)
    w("")

    w("=" * 92)
    w("3. 现有节点的真实引脚名")
    w("=" * 92)
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    if bp is None:
        w("!! 载入不了 %s" % BP_PATH)
        flush()
        return
    try:
        ed = unreal.BlueprintGraphEditor.get_graph_editor_by_name(bp, GRAPH)
    except Exception as exc:
        w("!! 取不到图编辑器：%s" % str(exc)[:80])
        ed = None
    if ed is None:
        w("!! 没有 %s 这张图" % GRAPH)
        flush()
        return

    nodes = unreal.BlueprintGraphEditor.list_all_nodes(ed)
    w("图 %s 共 %d 个节点" % (GRAPH, len(nodes)))
    w("")
    for nd in nodes:
        try:
            title = unreal.BlueprintEditorLibrary.get_node_title(nd)
        except Exception:
            title = "?"
        title = str(title)
        # 只关心要动的那几个
        if not any(k in title for k in
                   ("Sphere Trace", "创建数组", "Make Array", "分支",
                    "Branch", "BreakHitResult", "Cast To")):
            continue
        w("--- %s   (%s)" % (title, nd.get_class().get_name()))
        try:
            pins = unreal.BlueprintEditorLibrary.list_all_pins(nd)
        except Exception as exc:
            w("    引脚列不出来：%s" % str(exc)[:60])
            continue
        for p in pins:
            try:
                nm = str(unreal.BlueprintGraphPin.get_pin_name(p))
                di = str(unreal.BlueprintGraphPin.get_pin_direction(p))
                conn = unreal.BlueprintGraphPin.list_connected_pins(p)
                tgt = []
                for cp in conn or []:
                    on = unreal.BlueprintGraphPin.get_owning_node(cp)
                    tn = str(unreal.BlueprintEditorLibrary.get_node_title(on))
                    tgt.append("%s.%s"
                               % (tn[:22],
                                  str(unreal.BlueprintGraphPin.get_pin_name(cp))))
            except Exception as exc:
                w("    !! 引脚读取失败：%s" % str(exc)[:60])
                continue
            w("    %-28s %-26s %s" % (nm, di, ", ".join(tgt)))
        w("")
        flush()

    w("=" * 92)
    w("4. 图里可用的节点名（只列名字含 trace / foreach 的）")
    w("=" * 92)
    try:
        avail = unreal.BlueprintGraphEditor.list_available_nodes(ed)
        hit = [str(x) for x in avail
               if "race" in str(x) or "ForEach" in str(x) or "foreach" in str(x)]
        w("   共 %d 个可用节点，其中相关的 %d 个：" % (len(avail), len(hit)))
        for x in hit[:40]:
            w("      %s" % x)
    except Exception as exc:
        w("   list_available_nodes 调不动：%s" % str(exc)[:80])
        w("   那就用 add_call_function_node 直接按函数名建。")
    w("")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[gapi] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[gapi] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
