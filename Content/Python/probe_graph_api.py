# -*- coding: utf-8 -*-
"""探明蓝图图表编辑 API：能建什么节点、怎么建 Branch、引脚默认值能不能写。

为什么单独做一个探针
--------------------
`create_node_from_name` 对 Branch 三种写法全返回 None（不是抛异常，是找不到）。
Branch 不是函数而是 `K2Node_IfThenElse`，「分类|函数名」那种写法对它不适用。
继续猜名字的成本是每次一整轮编辑器往返，而把方法表列出来一次就够。

顺带探两件后面一定会用到的：
  · 引脚默认值有没有 setter（`>` 的 B 要填字面量 1）
  · 还有哪些建节点的方法（可能有专门建 Branch / Select / 宏的）

会创建节点来试，**试完立刻删掉、不保存**。
结果写到 Saved/probe_graph_api.txt
用法：py probe_graph_api.py
"""

import traceback

import unreal

BP_PATH = "/Game/PS2DEM/BP_Intersection"
GRAPH = "EventGraph"

# Branch 的候选名。前几个是「分类|名字」风格，后几个按类名/纯名字试。
BRANCH_NAMES = [
    "Utilities|Flow Control|Branch",
    "Flow Control|Branch",
    "Branch",
    "Utilities|Branch",
    "Flow Control|Branch (if)",
    "Branch (if)",
    "if",
    "IfThenElse",
    "K2Node_IfThenElse",
    "/Script/BlueprintGraph.K2Node_IfThenElse",
    "Utilities|Flow Control|Branch (if)",
    "Control Flow|Branch",
]

OUT = unreal.Paths.project_saved_dir() + "probe_graph_api.txt"
lines = []
BEL = unreal.BlueprintEditorLibrary
BGE = getattr(unreal, "BlueprintGraphEditor", None)


def w(s=""):
    lines.append(str(s))
    unreal.log("[probe] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def methods(obj, keywords):
    out = []
    for n in dir(obj):
        if n.startswith("_"):
            continue
        low = n.lower()
        if any(k in low for k in keywords):
            fn = getattr(obj, n, None)
            doc = (getattr(fn, "__doc__", "") or "").strip().replace("\n", " ")
            out.append(u"  %-40s %s" % (n, doc[:130]))
    return out


def run():
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    w(u"=== 蓝图图表 API 探针 ===")
    if bp is None or BGE is None:
        w(u"！加载不到蓝图或没有 BlueprintGraphEditor")
        return
    ge = None
    for how in (lambda: BGE.get_graph_editor_by_name(bp, GRAPH),
                lambda: BGE.get_graph_editor(BEL.find_graph(bp, GRAPH))):
        try:
            ge = how()
            if ge is not None:
                break
        except Exception:
            continue
    if ge is None:
        w(u"！取不到 graph editor")
        return

    w(u"")
    w(u"① BlueprintGraphEditor 上和建/删节点有关的方法")
    w(u"=" * 72)
    for ln in methods(ge, ["node", "create", "add", "remove", "spawn", "paste", "macro"]):
        w(ln)

    w(u"")
    w(u"② 引脚对象上和取值/赋值有关的方法（`>` 的 B 要填字面量 1）")
    w(u"=" * 72)
    sample_pin = None
    try:
        for nd in ge.list_all_nodes():
            ps = None
            for how in (lambda: nd.list_all_pins(), lambda: BEL.list_all_pins(nd)):
                try:
                    ps = how()
                    if ps:
                        break
                except Exception:
                    pass
            if ps:
                sample_pin = ps[0]
                break
    except Exception as exc:
        w(u"  取样引脚失败：%s" % str(exc)[:80])
    if sample_pin is None:
        w(u"  没取到样本引脚")
    else:
        for ln in methods(sample_pin, ["value", "default", "set", "literal"]):
            w(ln)

    w(u"")
    w(u"③ Branch 用哪个名字能建出来（建完立刻删）")
    w(u"=" * 72)
    winners = []
    for nm in BRANCH_NAMES:
        node, err = None, None
        try:
            node = ge.create_node_from_name(nm, unreal.Vector2D(0.0, 0.0), [], None)
        except Exception as exc:
            err = str(exc)[:90]
        if node is None:
            w(u"  %-46s %s" % (nm, u"返回 None" if err is None else u"抛了 " + err))
            continue
        # **有引脚才算真建出来**：没引脚的空节点只会在编译时报错
        ps = []
        for how in (lambda: node.list_all_pins(), lambda: BEL.list_all_pins(node)):
            try:
                ps = how() or []
                if ps:
                    break
            except Exception:
                pass
        names = [str(p.get_pin_name()) for p in ps]
        ok = ("Condition" in names) and ("then" in names)
        w(u"  %-46s 建出来了，引脚 [%s] %s"
          % (nm, u", ".join(names), u"← 就是它" if ok else u"（不像 Branch）"))
        if ok:
            winners.append(nm)
        try:
            ge.remove_nodes([node])
        except Exception:
            w(u"      ！删不掉，图里留了个节点，关编辑器别保存")

    w(u"")
    if winners:
        w(u"可用的 Branch 名字：%s" % winners[0])
        w(u"把它填进 fix_single_group_intersection.py 就能接着做。")
    else:
        w(u"没有一个名字能建出 Branch。看①里列出的方法表，")
        w(u"可能有专门建节点的别的入口（比如按类建、或者粘贴节点文本）。")
    w(u"")
    w(u"全程没有保存。图可能被标脏但内容已还原，关编辑器时选不保存。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[probe] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[probe] 写入 %s" % OUT)
