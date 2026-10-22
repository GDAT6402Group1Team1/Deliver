# -*- coding: utf-8 -*-
"""修正上一轮：建的是同名「自定义事件」而不是 C++ 事件的实现，手机因此打不开。

上一轮（fix_phone_cursor.py）做对了三件事：删了 IA_TogglePhone、删了光标三件套、
接了执行线。但第 1 步建事件节点时三种形状里中的是 add_custom_event_node，
建出来的是 K2Node_CustomEvent（标题「自定义事件」）——那是一个**新函数**，
C++ 的 TogglePhoneUI() 不会调到它。于是现在按 J 的结果是：
C++ 翻状态、光标变了，但面板不会开。比改之前更糟，必须立刻修。

上一轮的报告已经把正确签名打出来了：
    add_event_override(blueprint, event_name, position) -> K2Node_Event
    position 是 **IntPoint**，不是 Vector2D
我当时传的是 Vector2D，所以第一种形状报 required argument 'position' not found，
才滑到了 add_custom_event_node 那条兜底上。

教训：判断「事件节点可用」不能只看有没有 then 引脚——自定义事件也有 then。
要看**节点类型是不是 K2Node_Event**，以及标题里是不是那个函数名。
本脚本两条都查。

做三件事：
  1. 删掉上一轮那个假的自定义事件节点（按类型 + 标题认，不按名字猜）
  2. 用正确签名建真的事件覆盖节点
  3. 接到 TogglePhone 调用上，编译存盘

顺序同样是「先建新的、验证通过再删旧的」。但这次旧的那个是**有害**的（同名自定义
事件可能和继承来的函数打架），所以两者都要在同一次里完成，中途失败会明确点名。

中文里一律用「」不用半角引号（这个脚本家族因为这个连续栽过三次，解析期报错、
整个脚本一行都不执行）。

用法：py fix_phone_event.py
"""

import traceback

import unreal

PC_BP = "/Game/Blueprint/PlayerController/BP_DeliveryManPC"
GRAPH = "EventGraph"
EVENT_NAME = "TogglePhoneUI"

OUT = unreal.Paths.project_saved_dir() + "fix_phone_event.txt"
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
    except Exception as exc:
        return "<title failed: " + str(exc) + ">"


def pins(n):
    try:
        return list(BEL.list_all_pins(n))
    except Exception:
        return []


def find_pin(n, name):
    for p in pins(n):
        if pin_name(p) == name:
            return p
    return None


def conn_nodes(p):
    out = []
    try:
        for other in BGP.list_connected_pins(p):
            out.append(BGP.get_owning_node(other))
    except Exception:
        pass
    return out


def main():
    bp = unreal.EditorAssetLibrary.load_asset(PC_BP)
    if not bp:
        log("加载失败：" + PC_BP)
        return

    editor = BGE.get_graph_editor_by_name(bp, GRAPH)
    nodes = list(BGE.list_all_nodes(editor))
    log("现有 " + str(len(nodes)) + " 个节点（上一轮之后是 10）")
    log()
    log("=" * 70)
    log("现状")
    log("=" * 70)
    for n in nodes:
        log("  [" + n.get_class().get_name() + "] " + n.get_name() + "  " + node_title(n))

    toggle_call = None
    bogus = []
    existing_event = None
    for n in nodes:
        cls = n.get_class().get_name()
        if cls == "K2Node_CallFunction" and node_title(n) == "TogglePhone":
            toggle_call = n
        if cls == "K2Node_CustomEvent":
            bogus.append(n)
        if cls == "K2Node_Event" and EVENT_NAME.lower() in node_title(n).lower():
            existing_event = n

    log()
    log("TogglePhone 调用 = " + (toggle_call.get_name() if toggle_call else "没找到"))
    log("自定义事件（要删的）= " + str([n.get_name() for n in bogus]))
    log("已有的真事件节点 = " + (existing_event.get_name() if existing_event else "没有"))

    if not toggle_call:
        log("找不到 TogglePhone 调用，中止。")
        return

    # ---------------------------------------------------------------- 建真事件
    log()
    log("=" * 70)
    log("1. 用正确签名建事件覆盖节点")
    log("=" * 70)

    event_node = existing_event
    if event_node:
        log("   已经有了，直接用：" + event_node.get_name())
    else:
        try:
            # position 是 IntPoint，不是 Vector2D——上一轮就栽在这里
            event_node = BEL.add_event_override(bp, EVENT_NAME, unreal.IntPoint(-400, 900))
        except Exception as exc:
            log("   add_event_override 抛异常：" + str(exc))
            event_node = None

        if not event_node:
            log("   返回空。文档说明的三种失败原因：这个函数不是可覆盖的 event-shape、")
            log("   蓝图没有事件图、或者蓝图无效。前者最可能——那就得确认")
            log("   TogglePhoneUI 确实是 BlueprintImplementableEvent 且 C++ 已编译进编辑器。")
            log("   **什么都没改，先别删那个自定义事件**，否则手机连现在这个坏状态都回不去。")
            return

        cls = event_node.get_class().get_name()
        log("   建出来：" + event_node.get_name() + " [" + cls + "] " + node_title(event_node))
        # 两条判据都要查：自定义事件也有 then 引脚，光看 then 会再次误判
        if cls != "K2Node_Event":
            log("   类型不是 K2Node_Event，还是不对。删掉、中止。")
            try:
                editor.remove_nodes([event_node])
            except Exception:
                pass
            return
        if not find_pin(event_node, "then"):
            log("   没有 then 引脚，删掉、中止。")
            try:
                editor.remove_nodes([event_node])
            except Exception:
                pass
            return
        log("   OK：类型是 K2Node_Event 且有 then 引脚")

    # ---------------------------------------------------------------- 接线
    log()
    log("=" * 70)
    log("2. 接 " + EVENT_NAME + ".then -> TogglePhone.execute")
    log("=" * 70)
    then_pin = find_pin(event_node, "then")
    exec_pin = find_pin(toggle_call, "execute")
    ok = False
    try:
        BGP.try_create_connection(then_pin, exec_pin)
        ok = len(conn_nodes(exec_pin)) > 0
    except Exception as exc:
        log("   try_create_connection 抛异常：" + str(exc))
    log("   " + ("OK，连上了" if ok else "失败：调用没报错但连线没出现"))
    if not ok:
        log("   不删自定义事件——留着它至少说明图是上一轮那个状态，别再退一步。")
        return

    # ---------------------------------------------------------------- 删假事件
    log()
    log("=" * 70)
    log("3. 删掉那个同名自定义事件")
    log("=" * 70)
    if bogus:
        log("   要删：" + str([n.get_name() for n in bogus]))
        try:
            editor.remove_nodes(bogus)
            log("   remove_nodes 完成")
        except Exception as exc:
            log("   失败：" + str(exc))
            log("   必须手工删：同名自定义事件会和继承来的函数打架。")
    else:
        log("   没有，跳过")

    # ---------------------------------------------------------------- 编译存盘
    log()
    log("=" * 70)
    log("4. 编译 + 存盘")
    log("=" * 70)
    after = list(BGE.list_all_nodes(editor))
    log("   现在 " + str(len(after)) + " 个节点")
    for n in after:
        log("     [" + n.get_class().get_name() + "] " + n.get_name() + "  " + node_title(n))
    try:
        BEL.compile_blueprint(bp)
        status = bp.get_editor_property("status")
        log("   编译状态 = " + str(status))
        if "ERROR" in str(status).upper():
            log("   报错了。打开 BP_DeliveryManPC 看编译器输出。")
            return
    except Exception as exc:
        log("   编译失败：" + str(exc))
        return
    try:
        bp.modify(True)
        log("   存盘 = " + str(unreal.EditorAssetLibrary.save_loaded_asset(
            bp, only_if_is_dirty=False)))
    except Exception as exc:
        log("   存盘失败：" + str(exc))

    log()
    log("验收（要先把 C++ 编译进去）：按 J 手机开、出现光标；再按 J 关、光标消失、")
    log("鼠标立刻能转视角；骑车时反复开关几次鼠标都不该失灵。")


try:
    main()
except Exception:
    log("整段抛异常：")
    log(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines))
print()
print("报告：" + OUT)
