# -*- coding: utf-8 -*-
"""删掉 WBP_Compass / UpdateCompass 里孤立的 VariableGet 节点。

改成"收根面板"之后，原来喂那五个 SetVisibility 的 Get 节点没有消费者了。
纯数据节点（没有执行引脚）输出不接任何东西时**不产生任何代码**，编译器直接忽略，
所以删不删都不影响运行时——纯粹是让图能看懂。

**判据只有一条：输出引脚没有任何连线。** 这条是自我保护的——
如果某个 Get 节点其实还在喂 SetRenderTranslation（管罗盘字母位置那条链），
它就**有**连线，自然不会被选中。
上一轮差点出事的地方正是判据选错了：当时用"标题是 `Get Compass_N`"，
而 `Compass_N` 有**两个** Get 节点（一个喂位置、一个喂可见性），
按名字删会把管位置的那个一起删掉。按连线判没有这个失败模式。

先把每个候选的名字和全部引脚连线状态打出来，再删。
用法：py clean_compass_orphans.py
"""

import traceback

import unreal

TARGET = "/Game/UI/Minimap/WBP_Compass"
GRAPH = "UpdateCompass"

OUT = unreal.Paths.project_saved_dir() + "clean_compass_orphans.txt"
lines = []


def log(s=""):
    lines.append(str(s))


BEL = unreal.BlueprintEditorLibrary
BGE = unreal.BlueprintGraphEditor
BGP = unreal.BlueprintGraphPin


def pin_name(p):
    return str(BGP.get_pin_name(p))


def node_title(n):
    try:
        return str(BEL.get_node_title(n))
    except Exception:
        return repr(n)


def main():
    bp = unreal.EditorAssetLibrary.load_asset(TARGET)
    if not bp:
        log("加载失败：%s" % TARGET)
        return

    editor = BGE.get_graph_editor_by_name(bp, GRAPH)
    nodes = list(BGE.list_all_nodes(editor))
    log("图里现有 %d 个节点" % len(nodes))
    log()

    gets = [n for n in nodes if n.get_class().get_name() == "K2Node_VariableGet"]
    log("=" * 70)
    log("全部 %d 个 VariableGet 节点的连线状态" % len(gets))
    log("=" * 70)

    orphans = []
    for n in gets:
        total = 0
        detail = []
        try:
            for p in BEL.list_all_pins(n):
                try:
                    conn = list(BGP.list_connected_pins(p))
                except Exception:
                    conn = []
                total += len(conn)
                tgt = [("%s.%s" % (node_title(BGP.get_owning_node(c)), pin_name(c)))
                       for c in conn]
                detail.append("%s -> %s" % (pin_name(p), ", ".join(tgt) if tgt else "（空）"))
        except Exception as exc:
            log("  %s 引脚读不到：%s —— 跳过，不删" % (n.get_name(), exc))
            continue

        log()
        log("  %s  %s   连线总数 %d%s"
            % (n.get_name(), node_title(n), total, "   ← 孤立，可删" if total == 0 else ""))
        for d in detail:
            log("      %s" % d)
        if total == 0:
            orphans.append(n)

    log()
    log("=" * 70)
    log("删除")
    log("=" * 70)
    if not orphans:
        log("没有孤立节点，图已经是干净的。")
        return

    log("要删 %d 个：%s" % (len(orphans), [n.get_name() for n in orphans]))
    try:
        editor.remove_nodes(orphans)
        log("remove_nodes 完成")
    except Exception as exc:
        log("失败：%s" % exc)
        return

    after = list(BGE.list_all_nodes(editor))
    log("删除后 %d 个节点（预期 %d）%s"
        % (len(after), len(nodes) - len(orphans),
           "✓" if len(after) == len(nodes) - len(orphans) else "  ← 数字不对"))

    try:
        BEL.compile_blueprint(bp)
        status = bp.get_editor_property("status")
        log("编译状态 = %s" % status)
        if "ERROR" in str(status).upper():
            log("**报错了**——说明删错了东西。立刻在编辑器里对 WBP_Compass 做 Revert，")
            log("别保存。然后把这份报告发回去。")
            return
    except Exception as exc:
        log("编译失败：%s" % exc)
        return

    try:
        bp.modify(True)
        log("存盘 = %s" % unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False))
    except Exception as exc:
        log("存盘失败：%s" % exc)

    log()
    log("行为不该有任何变化——删的是不产生代码的纯数据节点。")
    log("验收还是那三条：不接电话罗盘条整条不出现 / 接通后指向取件点 / 取货后指向收件点。")


try:
    main()
except Exception:
    log("整段抛异常：")
    log(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines))
print()
print("报告：%s" % OUT)
