# -*- coding: utf-8 -*-
"""编译 WBP_Compass 并验证罗盘条那次改动真的落住了。

接在 gate_compass_on_task.py + gate_compass_sides.py 之后跑。
脚本加的节点**不编译不生效**，而且编译器会把接错的线直接报出来——
比肉眼看图可靠得多，所以这一步不能省。

做四件事，全部只读/只编译，不改图：
  1. 数 UpdateCompass 的节点（基准 35 = 原 27 + 新加 8）
  2. 核对那五个 SetVisibility 的 self 和 InVisibility 接到了谁
  3. 编译蓝图，读编译状态
  4. 存盘

**重启编辑器后再跑一次，比两次的数字**——这是本项目验证"存住了没有"的标准动作
（见 CLAUDE.md「编辑器 Python 写数据的三条边界」第 1 条：脚本说"写入成功"
只能证明函数被调用过，证明不了结果留住了）。

用法：py compile_compass.py
"""

import traceback

import unreal

TARGET = "/Game/UI/Minimap/WBP_Compass"
GRAPH = "UpdateCompass"
EXPECTED_NODES = 35
EXPECTED_SIDES = ["Compass_N", "Compass_S", "Compass_E", "Compass_W", "Compass_Target"]

OUT = unreal.Paths.project_saved_dir() + "compile_compass.txt"
lines = []


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
    except Exception:
        return repr(n)


def connections(p):
    out = []
    try:
        for other in BGP.list_connected_pins(p):
            owner = BGP.get_owning_node(other)
            out.append("%s.%s" % (node_title(owner), pin_name(other)))
    except Exception as exc:
        out.append("<取不到: %s>" % exc)
    return out


def main():
    bp = unreal.EditorAssetLibrary.load_asset(TARGET)
    if not bp:
        log("加载失败：%s" % TARGET)
        return

    log("WBP_Compass 编译 + 验证")
    log()

    # ---------------------------------------------------------------- 1. 节点数
    editor = BGE.get_graph_editor_by_name(bp, GRAPH)
    nodes = list(BGE.list_all_nodes(editor))
    log("=" * 70)
    log("1. 节点数")
    log("=" * 70)
    log("%s 图里 %d 个节点（基准 %d = 原 27 + 新加 8）%s"
        % (GRAPH, len(nodes), EXPECTED_NODES,
           "✓" if len(nodes) == EXPECTED_NODES else "  ← 不对！"))
    if len(nodes) < EXPECTED_NODES:
        log("少了说明加的节点没存住——重跑 gate_compass_sides.py。")
    elif len(nodes) > EXPECTED_NODES:
        log("多了说明重复跑过加节点脚本，图里有多余的 SetVisibility。")
        log("多出来的那几个是重复设同一个值，不影响行为，但该用 remove_nodes 清掉。")

    # ---------------------------------------------------------------- 2. 接线
    log()
    log("=" * 70)
    log("2. 五个 SetVisibility 的接线")
    log("=" * 70)

    set_vis = [n for n in nodes if "SetVisibility" in node_title(n)]
    log("找到 %d 个 SetVisibility（应为 5）%s"
        % (len(set_vis), "✓" if len(set_vis) == 5 else "  ← 不对！"))

    covered = []
    for n in set_vis:
        log()
        log("  %s" % n.get_name())
        for want in ("self", "InVisibility", "execute", "then"):
            p = next((x for x in BEL.list_all_pins(n) if pin_name(x) == want), None)
            if p is None:
                log("    %s：**没有这个引脚**（函数没解析上？）" % want)
                continue
            conn = connections(p)
            log("    %s -> %s" % (want, ", ".join(conn) if conn else "（没接）"))
            if want == "self":
                for name in EXPECTED_SIDES:
                    if any(name in c for c in conn):
                        covered.append(name)

    log()
    missing = [x for x in EXPECTED_SIDES if x not in covered]
    log("已接上可见性的控件：%s" % (sorted(set(covered)) or "无"))
    log("缺的：%s" % (missing if missing else "无 ✓"))

    # ---------------------------------------------------------------- 3. 编译
    log()
    log("=" * 70)
    log("3. 编译")
    log("=" * 70)

    try:
        BEL.compile_blueprint(bp)
        log("compile_blueprint() 调用完成")
    except Exception as exc:
        log("compile_blueprint 失败：%s" % exc)
        log("BlueprintEditorLibrary 上和编译有关的函数：%s"
            % [d for d in dir(BEL) if "compil" in d.lower()])

    # 编译状态：BS_UpToDate 才算干净；BS_Error 就是接错了线
    ok, status = (True, None), None
    try:
        status = bp.get_editor_property("status")
        log("编译状态 = %s" % status)
        if "ERROR" in str(status).upper():
            log("**编译报错**——接错了线。去编辑器里打开 WBP_Compass 看编译器输出，")
            log("它会点名是哪个节点。必要时用 remove_nodes 删掉新加的 8 个重来。")
        elif "UP_TO_DATE" in str(status).upper() or "UPTODATE" in str(status).upper():
            log("✓ 干净编译通过")
    except Exception as exc:
        log("读不到编译状态（%s）——去编辑器里看 WBP_Compass 有没有报错标记。" % exc)

    # ---------------------------------------------------------------- 4. 存盘
    log()
    log("=" * 70)
    log("4. 存盘")
    log("=" * 70)
    try:
        bp.modify(True)
        saved = unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False)
        log("save_loaded_asset = %s" % saved)
    except Exception as exc:
        log("存盘失败：%s" % exc)

    log()
    log("**重启编辑器后再跑一次这个脚本、比两次的节点数**——")
    log("脚本说成功只能证明函数被调用过，证明不了结果留住了。")
    log()
    log("验收：不接电话 → 罗盘条整条不出现；接通任务电话 → 出现并指向取件点；")
    log("      取到货 → 转为指向收件点。")


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
