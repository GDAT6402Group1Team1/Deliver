# -*- coding: utf-8 -*-
"""导出 WBP_TaskHUD 的全部图表（节点 + 连线），只读。

在编辑器控制台（Python 模式）跑：
    import importlib, dump_task_hud as d; importlib.reload(d); d.run()

要查的是**手机面板的倒计时在哪被按状态挡住了**。
C++ 侧已经确认是对的：`GetTimeSnapshot()` 对「已接听但还没取件」的任务
返回 `bRunning = True`，顶部任务条（`DeliveryTaskHudComponent.cpp:240`）
也放行了 `AwaitingPickup`。而这个控件同时引用了 `InProgress` 和
`AwaitingPickup` 两个枚举值，所以挡住倒计时的判断大概率在这儿。

**连线必须一起导**：只看节点标题分不出"谁喂给谁"，而要改的正是某个分支的判据。
罗盘那次就是靠连线才找到执行链尾巴的。
"""

import unreal


BP_PATHS = ["/Game/UI/Task/WBP_TaskHUD", "/Game/UI/Task/WBP_TaskRow"]

OUT = unreal.Paths.project_saved_dir() + "task_hud_graphs.txt"
_lines = []


def w(text=""):
    print(text)
    _lines.append(str(text))


def pin_names(node):
    """节点的引脚名。get_pin_name() 返回的是 unreal.Name 不是 str，要显式转。"""
    out = []
    for p in unreal.BlueprintEditorLibrary.list_all_pins(node) or []:
        try:
            out.append(str(p.get_pin_name()))
        except Exception as exc:
            out.append("<读不到:%s>" % str(exc)[:30])

    return out


def node_title(node):
    try:
        return str(unreal.BlueprintEditorLibrary.get_node_title(node))
    except Exception:
        return "<取不到标题>"


def dump_graph(bp, name):
    editor = unreal.BlueprintGraphEditor.get_graph_editor_by_name(bp, name)
    if not editor:
        w("  （拿不到 %s 的图表编辑器）" % name)

        return

    nodes = unreal.BlueprintGraphEditor.list_all_nodes(editor) or []
    w("--- %s：%d 个节点 ---" % (name, len(nodes)))
    for i, n in enumerate(nodes):
        w("  [%2d] %-44s 引脚: %s" % (i, node_title(n)[:44], ", ".join(pin_names(n))[:90]))
        for pin in unreal.BlueprintEditorLibrary.list_all_pins(n) or []:
            links = unreal.BlueprintGraphPin.list_connected_pins(pin) or []
            if not links:
                continue
            tgt = []
            for lp in links:
                owner = unreal.BlueprintGraphPin.get_owning_node(lp)
                tgt.append("%s.%s" % (node_title(owner)[:24], lp.get_pin_name()))
            w("        %-20s -> %s" % (str(pin.get_pin_name()), ", ".join(tgt)[:90]))
    w("")


def run():
    for BP_PATH in BP_PATHS:
        _dump_one(BP_PATH)
    _write()


def _dump_one(BP_PATH):
    w("")
    w("########## %s ##########" % BP_PATH)
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    if not bp:
        w("!! 载不到 %s" % BP_PATH)

        return

    graphs = unreal.BlueprintEditorLibrary.list_graphs(bp) or []
    w("蓝图 %s 共 %d 张图表" % (BP_PATH, len(graphs)))
    w("")

    # list_graphs 返回的是图表对象，名字不好直接取；按已知的几张逐个试，
    # 再把 list_graphs 的数量打出来做对照——数量对不上说明还有没覆盖到的图
    candidates = ["EventGraph", "RefreshTaskList", "RefreshPhone", "RefreshCounter",
                  "TogglePhone", "SetPhoneOpen", "UserConstructionScript"]
    found = 0
    for name in candidates:
        if unreal.BlueprintEditorLibrary.find_graph(bp, name):
            found += 1
            dump_graph(bp, name)

    w("按名字命中 %d 张，list_graphs 报告 %d 张" % (found, len(graphs)))
    if found < len(graphs):
        w("!! 有图表没被覆盖到。在编辑器左侧看一下函数列表里还有哪些名字，"
          "加进脚本顶部的 candidates 再跑一次。")


def _write():
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(_lines))
    unreal.log("[TaskHUD] 报告写到 %s" % OUT)


if __name__ == "__main__":
    run()
