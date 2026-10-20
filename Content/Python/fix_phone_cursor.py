# -*- coding: utf-8 -*-
"""把手机 UI 的光标控制从蓝图交给 C++，并查清 WBP_TaskHUD 的两个遗留问题。

要修的 bug：骑车时鼠标转不了视角，必须按住左键或右键。

根因（diag_input_mode.py 导出图表确认）：BP_DeliveryManPC 的 EventGraph 里
    IA_TogglePhone(Started) -> WBP_Main.TogglePhone                     面板开关
                           -> Set bShowMouseCursor = NOT(Get bShowMouseCursor) 光标开关
是两个互不相干的取反开关。全工程只有这一处写 bShowMouseCursor（C++ 里一处都没有），
所以只要有一次只翻了其中一个，两者就永久失同步；光标留在开着的状态时鼠标移动归光标，
视口只在按住鼠标键时才捕获鼠标，于是鼠标看起来失灵。

C++ 侧已改成「光标 = 有没有 UI 面板开着」：ADeliveryPlayerController::SetUIPanelOpen
维护一个面板集合，在「有/没有」翻转时设 bShowMouseCursor 和输入模式。
按 J 时 C++ 自己翻 bPhoneUIOpen、更新集合，然后才调蓝图事件 TogglePhoneUI。
所以蓝图必须改三处，否则会按一次翻两遍：

  1. 删掉 Set/Get bShowMouseCursor 和中间那个 NOT，光标归 C++ 了
  2. 删掉 IA_TogglePhone 节点。C++ 的 TogglePhoneKey(J) 已经在管这个键，
     两条路都在的话一次按键翻两遍，表现是按 J 没反应。
     这也正是 CLAUDE.md 里要求、但一直没做的那步迁移（IMC_Default 老被 git 冲掉）。
  3. 实现 TogglePhoneUI 事件，接到 WBP_Main.TogglePhone（面板显示/隐藏仍归蓝图）

顺序是刻意的：先把事件节点建出来并验证，最难的一步成功之后才删东西。
反过来一旦事件建不出来，就是「按键路径删了、新路径没有」，手机彻底打不开。

另外两段只读调查，一次跑完省得再来一轮：
  * WBP_TaskHUD.TogglePhone 内部怎么开关面板，有没有自己关掉自己的路径
    （有的话那里也要调 SetUIPanelOpen(false)，否则光标会留着，
     这是新设计唯一需要调用方配合的地方）
  * WBP_TaskHUD 在两个路径下各有一份真资产（314KB / 318KB，都不是重定向器），
    用资产注册表查引用，确定哪份在用

本文件的中文里一律用「」，不用半角引号：同一个脚本因为「中文里的半角引号嵌在
双引号字符串里」连续栽过三次，而这种错是解析期报错、整个脚本一行都不执行。

用法：py fix_phone_cursor.py
"""

import traceback

import unreal

PC_BP = "/Game/Blueprint/PlayerController/BP_DeliveryManPC"
GRAPH = "EventGraph"
EVENT_NAME = "TogglePhoneUI"
TASKHUD_PATHS = ["/Game/UI/Task/WBP_TaskHUD", "/Game/Task/UI/WBP_TaskHUD"]

OUT = unreal.Paths.project_saved_dir() + "fix_phone_cursor.txt"
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


def conn_labels(p):
    out = []
    try:
        for other in BGP.list_connected_pins(p):
            owner = BGP.get_owning_node(other)
            out.append(node_title(owner) + "." + pin_name(other))
    except Exception:
        pass
    return out


def dump_node(n, indent):
    pad = " " * indent
    log(pad + "[" + n.get_name() + "] " + node_title(n))
    for p in pins(n):
        val = ""
        try:
            v = BGP.get_pin_value(p)
            if v not in (None, ""):
                val = " = " + str(v)
        except Exception:
            pass
        tgt = conn_labels(p)
        tail = ""
        if tgt:
            tail = " -> " + ", ".join(tgt)
        log(pad + "    " + pin_name(p) + val + tail)


def wire(desc, a, b):
    if a is None or b is None:
        log("    FAIL " + desc + " 引脚找不到")
        return False
    try:
        BGP.try_create_connection(a, b)
        # try_create_connection 可能静默失败，必须回读
        if conn_nodes(b):
            log("    OK   " + desc)
            return True
        log("    FAIL " + desc + " 调用没报错但连线没出现")
    except Exception as exc:
        log("    FAIL " + desc + " " + str(exc))
    return False


def investigate_taskhud():
    log("=" * 70)
    log("调查 1：WBP_TaskHUD.TogglePhone 内部，找有没有自己关掉自己的路径")
    log("=" * 70)

    for path in TASKHUD_PATHS:
        log()
        log("--- " + path + " ---")
        bp = unreal.EditorAssetLibrary.load_asset(path)
        if not bp:
            log("加载失败，这份可能不存在")
            continue
        try:
            graphs = [str(g.get_name()) for g in BEL.list_graphs(bp)]
        except Exception as exc:
            log("list_graphs 失败：" + str(exc))
            continue
        log("图：" + str(graphs))

        for gname in graphs:
            if "toggle" not in gname.lower() and gname != "EventGraph":
                continue
            try:
                editor = BGE.get_graph_editor_by_name(bp, gname)
                nodes = list(BGE.list_all_nodes(editor))
            except Exception as exc:
                log("  " + gname + " 取节点失败：" + str(exc))
                continue
            log()
            log("  graph " + gname + " / " + str(len(nodes)) + " nodes")
            for n in nodes:
                dump_node(n, 4)


def investigate_duplicates():
    log()
    log("=" * 70)
    log("调查 2：WBP_TaskHUD 两份资产谁在用")
    log("=" * 70)
    try:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()
    except Exception as exc:
        log("拿不到资产注册表：" + str(exc))
        return

    for path in TASKHUD_PATHS:
        log()
        log("--- " + path + " ---")
        refs = None
        attempts = [
            ("get_referencers(path, options)",
             lambda: ar.get_referencers(path, unreal.AssetRegistryDependencyOptions())),
            ("get_referencers(path)", lambda: ar.get_referencers(path)),
        ]
        for desc, fn in attempts:
            try:
                refs = fn()
                log("用 " + desc)
                break
            except Exception as exc:
                log(desc + " 失败：" + str(exc))
        if refs is None:
            continue
        refs = [str(r) for r in (refs or [])]
        if refs:
            log("被 " + str(len(refs)) + " 个资产引用：")
            for r in refs:
                log("    " + r)
        else:
            log("没有任何引用，这份大概就是可以删的那份")
            log("删之前仍要在编辑器 Reference Viewer 再确认，注册表对没加载过的资产有时不全")


def fix_player_controller():
    log()
    log("=" * 70)
    log("改 BP_DeliveryManPC：光标交给 C++")
    log("=" * 70)

    bp = unreal.EditorAssetLibrary.load_asset(PC_BP)
    if not bp:
        log("加载失败：" + PC_BP)
        return

    editor = BGE.get_graph_editor_by_name(bp, GRAPH)
    nodes = list(BGE.list_all_nodes(editor))
    log("现有 " + str(len(nodes)) + " 个节点")

    # 按连线认那三个光标节点，不按标题乱删：
    # 按标题删在罗盘条那次差点毁掉位置逻辑（同名 Get 节点有好几个）
    set_cursor = None
    for n in nodes:
        if node_title(n) == "Set bShowMouseCursor":
            set_cursor = n
            break
    if not set_cursor:
        log("找不到 Set bShowMouseCursor，可能已经改过了。先跑 diag_input_mode.py 看现状，中止。")
        return

    cursor_group = [set_cursor]
    in_pin = find_pin(set_cursor, "bShowMouseCursor")
    not_node = None
    for n in (conn_nodes(in_pin) if in_pin else []):
        if n.get_class().get_name() == "K2Node_CallFunction":
            not_node = n
            cursor_group.append(n)
    if not_node:
        a_pin = find_pin(not_node, "A")
        for n in (conn_nodes(a_pin) if a_pin else []):
            cursor_group.append(n)

    log("光标相关节点（按连线认出来的）：")
    for n in cursor_group:
        log("    " + n.get_name() + "  " + node_title(n))

    ia_node = None
    toggle_call = None
    for n in nodes:
        cls = n.get_class().get_name()
        if cls == "K2Node_EnhancedInputAction" and ia_node is None:
            ia_node = n
        if cls == "K2Node_CallFunction" and node_title(n) == "TogglePhone":
            toggle_call = n

    log("IA 节点 = " + (ia_node.get_name() if ia_node else "没找到"))
    log("TogglePhone 调用 = " + (toggle_call.get_name() if toggle_call else "没找到"))
    if not toggle_call:
        log("找不到 TogglePhone 调用，中止：没有它就没法把新事件接到面板开关上。")
        return

    log()
    log("1. 建 " + EVENT_NAME + " 事件节点")
    doc = getattr(getattr(BEL, "add_event_override", None), "__doc__", "<no such function>")
    log("   add_event_override.__doc__ = " + str(doc))

    event_node = None
    attempts = [
        ("BEL.add_event_override(bp, name)",
         lambda: BEL.add_event_override(bp, EVENT_NAME)),
        ("editor.create_node_from_name(AddEvent)",
         lambda: editor.create_node_from_name("Add Event|" + EVENT_NAME,
                                              unreal.Vector2D(-400.0, 900.0), [])),
        ("editor.add_custom_event_node(name)",
         lambda: editor.add_custom_event_node(EVENT_NAME)),
    ]
    for desc, fn in attempts:
        try:
            n = fn()
        except Exception as exc:
            log("   " + desc + " 失败：" + str(exc))
            continue
        if not n:
            log("   " + desc + " 返回空")
            continue
        # 有没有 then 执行引脚 = 这个事件节点是不是真的能用
        if find_pin(n, "then"):
            log("   OK " + desc + " -> " + n.get_name() + " / " + node_title(n))
            event_node = n
            break
        log("   " + desc + " 建出来但没有 then 引脚，不是可用事件节点，删掉")
        try:
            editor.remove_nodes([n])
        except Exception:
            pass

    if not event_node:
        log()
        log("   事件节点建不出来。到此为止，一个节点都没删，蓝图还是原样可用。")
        log("   手工做法（三步）：")
        log("     a. EventGraph 空白处右键搜 " + EVENT_NAME + "，加上这个事件")
        log("     b. 把它的执行线接到现有的 TogglePhone 调用上")
        log("     c. 删掉 IA_TogglePhone 节点，以及 Set/Get bShowMouseCursor 和中间的 NOT")
        log("   做完编译保存即可，C++ 那边已经就绪。")
        return

    log()
    log("2. 接 " + EVENT_NAME + ".then -> TogglePhone.execute")
    if not wire("event -> TogglePhone", find_pin(event_node, "then"),
                find_pin(toggle_call, "execute")):
        log("   接线失败，不删任何东西。新建的事件节点请手工删：" + event_node.get_name())
        return

    log()
    log("3. 删掉 IA 节点和光标三件套")
    doomed = []
    if ia_node:
        doomed.append(ia_node)
    doomed.extend(cursor_group)
    log("   要删：" + str([n.get_name() for n in doomed]))
    try:
        editor.remove_nodes(doomed)
        log("   remove_nodes 完成")
    except Exception as exc:
        log("   失败：" + str(exc))
        log("   必须手工删：留着 IA 节点会按一次翻两遍，留着光标节点会再次失同步。")

    log()
    log("4. 编译 + 存盘")
    log("   现在 " + str(len(list(BGE.list_all_nodes(editor)))) + " 个节点")
    try:
        BEL.compile_blueprint(bp)
        status = bp.get_editor_property("status")
        log("   编译状态 = " + str(status))
        if "ERROR" in str(status).upper():
            log("   报错了。打开 BP_DeliveryManPC 看编译器输出，它会点名哪个节点。")
            log("   最可能是 TogglePhone 的 self 引脚丢了，原来接的是 Get PhoneWidget。")
            return
    except Exception as exc:
        log("   编译失败：" + str(exc))
        return
    try:
        bp.modify(True)
        saved = unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False)
        log("   存盘 = " + str(saved))
    except Exception as exc:
        log("   存盘失败：" + str(exc))

    log()
    log("验收：按 J 开手机，出现光标、能点；再按 J 关掉，光标消失、鼠标立刻能转视角；")
    log("      骑车时反复开关几次，鼠标都不该再失灵。")


try:
    investigate_taskhud()
    investigate_duplicates()
    fix_player_controller()
except Exception:
    log("整段抛异常：")
    log(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines[-70:]))
print()
print("完整报告：" + OUT)
