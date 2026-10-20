# -*- coding: utf-8 -*-
"""查「骑车时鼠标转不了视角、必须按住左键或右键」的根因。

症状在 UE 里有个很典型的解释：**鼠标光标被打开了**
（`bShowMouseCursor = true` + 输入模式 `GameAndUI`/`UI`）。
那时鼠标移动归光标，视口只在按住某个鼠标键时才捕获鼠标——
所以"按住左键或右键才能转视角"。"骑着骑着"突然失效 = 中途有个 UI 开了光标没还回来。

C++ 侧已确认**一处 `SetShowMouseCursor` / `SetInputMode` 都没有**
（grep 整个 Source/Delivery 为空），所以一定在蓝图里。
最可能是手机 UI：它本来就要让人点屏幕、必然开光标，而**来电是周期性的**，
正好对上"骑着骑着就失效"。

本脚本把几个相关蓝图的图表全导出来，并把命中关键词的节点单独汇总。
只读，不改任何资产。
用法：py diag_input_mode.py
"""

import traceback

import unreal

TARGETS = [
    "/Game/Blueprint/PlayerController/BP_DeliveryManPC",
    # 真正的面板状态在这一层：BP_DeliveryManPC → WBP_Main.TogglePhone
    # → WBP_TaskHUD.TogglePhone。**两个路径下都有同名资产**，所以两个都导，
    # 顺便看清关卡里用的是哪一个（重复资产本身也可疑）。
    "/Game/UI/Task/WBP_TaskHUD",
    "/Game/Task/UI/WBP_TaskHUD",
    "/Game/UI/Task/WBP_Phone",
    "/Game/UI/WBP_Main",
]

# 命中这些关键词的节点会被单独汇总到报告末尾
KEYWORDS = [
    "mouse cursor", "showmousecursor", "show mouse",
    "input mode", "setinputmode", "game and ui", "ui only", "game only",
    "add to viewport", "remove from parent", "removefromparent",
    "set user focus", "focus",
    "add mapping context", "remove mapping context", "clear all mappings",
]

OUT = unreal.Paths.project_saved_dir() + "diag_input_mode.txt"
lines = []
hits = []


def log(s=""):
    lines.append(str(s))


BEL = unreal.BlueprintEditorLibrary
BGE = unreal.BlueprintGraphEditor
BGP = unreal.BlueprintGraphPin


def pin_name(p):
    # get_pin_name() 返回 unreal.Name 不是 str
    return str(BGP.get_pin_name(p))


def node_title(n):
    try:
        return str(BEL.get_node_title(n))
    except Exception as exc:
        return "<标题取不到: %s>" % exc


def dump_bp(path):
    log()
    log("=" * 70)
    log("蓝图：%s" % path)
    log("=" * 70)

    bp = unreal.EditorAssetLibrary.load_asset(path)
    if not bp:
        log("加载失败（路径对不上？跳过）")
        return
    log("类型：%s" % bp.get_class().get_name())

    try:
        graphs = BEL.list_graphs(bp)
    except Exception as exc:
        log("list_graphs 失败：%s" % exc)
        return

    names = []
    for g in graphs:
        try:
            names.append(str(g.get_name()))
        except Exception:
            pass
    log("%d 个图：%s" % (len(names), names))

    for gname in names:
        try:
            editor = BGE.get_graph_editor_by_name(bp, gname)
            nodes = list(BGE.list_all_nodes(editor))
        except Exception as exc:
            log("  图 %s 取节点失败：%s" % (gname, exc))
            continue

        log()
        log("-" * 70)
        log("图：%s（%d 个节点）" % (gname, len(nodes)))
        log("-" * 70)

        for n in nodes:
            title = node_title(n)
            low = title.lower()
            flagged = any(k in low for k in KEYWORDS)

            log("")
            log("  %s[%s] %s" % ("★ " if flagged else "", n.get_name(), title))
            if flagged:
                hits.append("%s / %s / %s  (%s)" % (path.split("/")[-1], gname, title,
                                                    n.get_name()))

            try:
                pins = list(BEL.list_all_pins(n))
            except Exception as exc:
                log("      引脚取不到：%s" % exc)
                continue

            for p in pins:
                try:
                    pn = pin_name(p)
                except Exception as exc:
                    pn = "<名字取不到: %s>" % exc

                val = ""
                try:
                    v = BGP.get_pin_value(p)
                    if v not in (None, ""):
                        val = " = %s" % v
                except Exception:
                    pass

                conn = []
                try:
                    for other in BGP.list_connected_pins(p):
                        owner = BGP.get_owning_node(other)
                        conn.append("%s.%s" % (node_title(owner), pin_name(other)))
                except Exception:
                    pass

                log("      %s%s%s" % (pn, val, (" -> " + ", ".join(conn)) if conn else ""))


def main():
    log("查「骑车时鼠标转视角失效、必须按住鼠标键」")
    log()
    log("假设：某处把 bShowMouseCursor 打开 + 输入模式切成 GameAndUI，之后没还回来。")
    log("C++ 侧已 grep 确认没有任何 SetShowMouseCursor / SetInputMode 调用，所以在蓝图里。")
    log("下面 ★ 标记的节点就是嫌疑。")

    for t in TARGETS:
        try:
            dump_bp(t)
        except Exception:
            log()
            log("### %s 整段抛异常：" % t)
            log(traceback.format_exc())

    log()
    log("=" * 70)
    log("命中关键词的节点汇总（★）")
    log("=" * 70)
    if hits:
        for h in hits:
            log("  %s" % h)
    else:
        log("  一个都没有。")
        log("  那说明光标不是在这几个蓝图里开的。下一步换方向查：")
        log("  * 是不是按过 P 切成了固定视角？那套镜头**鼠标本来就不参与**")
        log("    （ADeliveryMotorbike::LookInput 开头就 `if (!bFreeLookCamera) return;`），")
        log("    左下角会显示「按 P 切换视角（当前：固定）」——先确认那行提示写的是哪个。")
        log("  * 或者 IA_Look / IA_MouseLook 的 Trigger 配了鼠标键和弦（Chorded），")
        log("    那就得查 IMC_Default 和两个 IA 资产本身，不在图表里。")
        log("  * 或者 TARGETS 漏了真正加 UI 的那个蓝图——把它补进 TARGETS 重跑。")


try:
    main()
except Exception:
    log("整段抛异常：")
    log(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

# 控制台只打汇总，完整的看文件——图表节点可能上千行
print("\n".join(lines[:6]))
print()
print("命中汇总：")
for h in hits:
    print("  " + h)
if not hits:
    print("  （无）")
print()
print("完整报告：%s" % OUT)
