# -*- coding: utf-8 -*-
"""让顶部罗盘条只在"接到任务后"出现。

现状（dump_compass_widget.py 导出来的，见 Saved/compass_widget.txt）：
  UpdateCompass（事件Tick 每帧调）
    GetTrackedTaskCompass -> Break
      NorthOffset/EastOffset/SouthOffset/WestOffset -> *360 -> SetRenderTranslation
      TargetOffset -> *360 -3 -> SetRenderTranslation(Compass_Target)
      bHasTarget -> 选择(Option0=Visible, Option1=Visible) -> SetVisibility(Compass_Target)

两个问题：
1. **那个 Select 的两个选项都是 Visible**，等于没判断，Compass_Target 永远显示。
   Option 0 对应 bHasTarget == false，本该是隐藏。这就是"一开局罗盘就在"的直接原因。
2. Compass_N/S/E/W 四个压根没有可见性逻辑，只被挪位置，所以也一直在。

bHasTarget 正好就是想要的条件（见 DeliveryTaskTrackerComponent::GetTaskDestination）：
  Locked 和 Completed -> false；AwaitingPickup（指取件点）/ InProgress（指收件点）-> true。

**为什么不直接把整个控件（self）收起来**：Slate 不 tick Collapsed 的控件，
而驱动这一切的「事件Tick」就在这个控件自己身上——收起来之后 UpdateCompass 再也不跑，
bHasTarget 变 true 也永远恢复不了，表现是"罗盘再也不出现"，极难查。
所以保持控件本身可见，只隐藏里面那 5 个元素。

本脚本做两件事：
  A. 把 Select 的 Option 0 改成 Collapsed（纯改引脚默认值，低风险）
  B. 给 Compass_N/S/E/W 也接上同一个可见性

B 需要往图里加节点，而**加节点的 Python API 在本项目没验证过**。所以 B 做不成时
不硬凑，把探到的可用 API 和手工步骤打出来，剩下那几下手点更快也更可靠。

用法：py gate_compass_on_task.py
"""

import traceback

import unreal

TARGET = "/Game/UI/Minimap/WBP_Compass"
GRAPH = "UpdateCompass"
# bHasTarget == false 时给什么可见性。Collapsed 不占位；这几个元素都在
# 绝对定位的容器里，占不占位无所谓，所以取更彻底的 Collapsed。
HIDDEN_VALUE = "Collapsed"
SIDE_WIDGETS = ["Compass_N", "Compass_S", "Compass_E", "Compass_W"]

OUT = unreal.Paths.project_saved_dir() + "gate_compass.txt"
lines = []


def log(s=""):
    lines.append(str(s))


BEL = unreal.BlueprintEditorLibrary
BGP = unreal.BlueprintGraphPin


def pin_name(p):
    # get_pin_name() 返回 unreal.Name 不是 str，直接切片会抛
    # "'Name' object is not subscriptable"
    return str(BGP.get_pin_name(p))


def node_title(n):
    try:
        return str(BEL.get_node_title(n))
    except Exception:
        return repr(n)


def find_nodes(bp):
    editor = unreal.BlueprintGraphEditor.get_graph_editor_by_name(bp, GRAPH)
    return list(unreal.BlueprintGraphEditor.list_all_nodes(editor))


# ------------------------------------------------------------------ A：改 Select
def fix_select_option(nodes):
    log("=" * 70)
    log("A. 把 Select 的 Option 0（bHasTarget == false）改成 %s" % HIDDEN_VALUE)
    log("=" * 70)

    targets = [n for n in nodes if n.get_class().get_name() == "K2Node_Select"]
    if not targets:
        log("没找到 Select 节点，图可能已经被改过了。中止 A。")
        return False

    done = False
    for n in targets:
        log("")
        log("节点 %s（%s）" % (n.get_name(), node_title(n)))
        for p in BEL.list_all_pins(n):
            name = pin_name(p)
            if name != "Option 0":
                continue

            try:
                before = BGP.get_pin_value(p)
            except Exception as exc:
                before = "<读不到: %s>" % exc
            log("  Option 0 当前值 = %s" % before)

            # 找一个能写引脚默认值的函数。名字未知，先把候选打出来再逐个试，
            # 这样就算全挂，输出里也留下了可用 API，下一轮能直接对着写。
            setters = [d for d in dir(BGP)
                       if ("set" in d.lower() and ("value" in d.lower()
                                                   or "default" in d.lower()))]
            log("  BlueprintGraphPin 上可能能写值的函数：%s" % (setters or "<一个都没有>"))

            for fn in setters:
                try:
                    getattr(BGP, fn)(p, HIDDEN_VALUE)
                    after = BGP.get_pin_value(p)
                    log("  %s() 写入后读回 = %s" % (fn, after))
                    if str(after) == HIDDEN_VALUE:
                        log("  ✓ 成功")
                        done = True
                        break
                except Exception as exc:
                    log("  %s() 失败: %s" % (fn, exc))

            if not done:
                log("  ✗ 写不进去。手工改法：打开 WBP_Compass -> UpdateCompass，")
                log("    选中那个「选择」节点，把 Option 0 的下拉从 Visible 改成 %s。"
                    % HIDDEN_VALUE)

    return done


# ------------------------------------------------------------------ B：四个方向
def gate_side_widgets(bp, nodes):
    log()
    log("=" * 70)
    log("B. 给 %s 接上同一个可见性" % "/".join(SIDE_WIDGETS))
    log("=" * 70)

    spawners = [d for d in dir(unreal.BlueprintGraphEditor)
                if any(k in d.lower() for k in ("add", "spawn", "create", "duplicate", "paste"))]
    log("BlueprintGraphEditor 上和「加节点」有关的函数：%s" % (spawners or "<一个都没有>"))
    spawners2 = [d for d in dir(BEL)
                 if any(k in d.lower() for k in ("add", "spawn", "create", "duplicate", "paste"))]
    log("BlueprintEditorLibrary 上的：%s" % (spawners2 or "<一个都没有>"))

    if not spawners and not spawners2:
        log("")
        log("没有可用的加节点 API —— 这一步交给手点，比硬凑脚本可靠。手工步骤：")
        log("  1. 打开 WBP_Compass -> UpdateCompass 图")
        log("  2. 选中最后那个 SetVisibility 节点（Target 是 Compass_Target 的那个），")
        log("     Ctrl+C / Ctrl+V 复制出 4 个")
        log("  3. 4 个副本的 Target 分别接 Get Compass_N / Compass_S / Compass_E / Compass_W")
        log("     （从左侧 Variables 面板把这四个变量拖进图里就有 Get 节点）")
        log("  4. 4 个副本的 In Visibility 都接到那个「选择」节点的 ReturnValue 上")
        log("     —— 和原来那个 SetVisibility 共用同一个 Select，不要各自再建一个")
        log("  5. 执行线串在原 SetVisibility 后面，一个接一个")
        log("  6. 编译 + 保存")
        log("")
        log("验收：进游戏别接电话——罗盘条整条不该出现；")
        log("      接通第一个任务电话后它出现并指向取件点；取到货后指向收件点。")
        return False

    log("")
    log("探到了上面这些候选函数，但本项目从没验证过它们能真正加节点。")
    log("这一步先不自动做：加错节点比不加更难收拾（图里会留下接错线的垃圾节点）。")
    log("把这份输出发回去，下一轮按真实签名写。")
    return False


def main():
    log("罗盘条按任务状态显示 —— 改动脚本")
    log("目标：%s / 图：%s" % (TARGET, GRAPH))
    log()

    bp = unreal.EditorAssetLibrary.load_asset(TARGET)
    if not bp:
        log("加载失败，路径对不上？")
        return

    try:
        nodes = find_nodes(bp)
    except Exception:
        log("取节点失败：")
        log(traceback.format_exc())
        return

    log("图里共 %d 个节点" % len(nodes))
    log()

    changed = False
    try:
        changed = fix_select_option(nodes) or changed
    except Exception:
        log("A 段抛异常：")
        log(traceback.format_exc())

    try:
        gate_side_widgets(bp, nodes)
    except Exception:
        log("B 段抛异常：")
        log(traceback.format_exc())

    log()
    log("=" * 70)
    if changed:
        # 改之前必须 modify(True)：普通函数调用只改内存、不把对象标记为已修改，
        # 保存时整个对象不被序列化。现象是写完立刻读回是好的、存盘重载就变回去。
        try:
            bp.modify(True)
            unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False)
            log("已标记修改并存盘。**重启编辑器后再跑一次 dump_compass_widget.py 比一下值**，")
            log("确认 Option 0 真的留住了——脚本里的「写入成功」只能证明函数被调用过。")
        except Exception as exc:
            log("存盘失败: %s" % exc)
            log("手动在编辑器里编译 + 保存 WBP_Compass。")
    else:
        log("没有任何自动改动落地，按上面的手工步骤改。")


main()

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines))
print()
print("报告：%s" % OUT)
