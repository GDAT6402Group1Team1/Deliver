# -*- coding: utf-8 -*-
"""罗盘条收尾：收起**根面板**，一次把那条线也盖掉。

树的真实形状（dump_compass_tree.py 查到的）：

    CanvasPanel_19            ← 根面板
    ├── SizeBox_20
    │   └── CanvasPanel_97
    │       └── Compass_N/E/S/W/Target   ← 这五个已经接了可见性
    └── Image_138             ← 剩下那条线（Image，VISIBLE，没人管）

与其再给那条线单独接一个（变成 6 处同样的判断），不如收起根面板：
子元素连那条线一起没，判断从 5 处收敛到 1 处。

**收根面板是安全的**：Slate 不 tick `Collapsed` 的控件，但那只适用于 **UserWidget 自己**。
`CanvasPanel_19` 是 UserWidget **内部**的顶层面板，「事件Tick」挂在 UserWidget 上，
收它不受影响。（修正之前"绝对不能收根"的过宽结论。）

---- 上一版实测撞到的三件事，全部已处理 ----

1. **`bIsVariable` 是 protected，设不了**（`Property 'bIsVariable' ... is protected
   and cannot be set`）。所以"把根面板暴露成变量再 Get"这条路是死的，
   和红绿灯 `start color` 那条同一类。只能走 `GetWidgetFromName`。
2. **`add_call_function_node` 传裸名 `"GetWidgetFromName"` 解析不上**，
   建出来的节点一个引脚都没有（`SetVisibility` 用裸名碰巧能解析，不能推广）。
   所以这版**按几种完整路径依次试**，并用"有没有生成参数引脚"判断成没成。
   没引脚的节点会让蓝图编译报 `Could not find a function named`，
   所以开头先把这种垃圾节点清掉。
3. **绝对不能按"标题是 Get <变量名>"去删 Get 节点**：`Compass_N` 这些变量
   **各有两个 Get 节点**——一个喂 `SetRenderTranslation`（管位置），一个喂 `SetVisibility`。
   上一版的删除名单把前者也算进去了，真删下去罗盘的位置逻辑会整个断掉
   （报告里"Get 节点 10 个"就是这个，幸好接线先失败了）。
   这版**只删那五个 SetVisibility，一个 Get 都不碰**；留下的 Get 变成孤立节点，
   纯数据读、没有消费者，编译无害。

**顺序**：先接新节点并回读验证，**确认成功之后才删**旧的。
反过来一旦中途失败就是"旧的删了、新的没成"——罗盘条全程不显示，比现在这条线难查得多。
（exec 输出引脚只能连一处，所以把上游接到新节点时，旧链会自动断开，删除只是清理。）

用法：py gate_compass_root.py
"""

import traceback

import unreal

TARGET = "/Game/UI/Minimap/WBP_Compass"
GRAPH = "UpdateCompass"
ROOT_WIDGET = "CanvasPanel_19"
OLD_GATED = ["Compass_N", "Compass_S", "Compass_E", "Compass_W", "Compass_Target"]

# 按顺序试，先中的先用。裸名已实测不行。
GET_WIDGET_PATHS = [
    "/Script/UMG.UserWidget:GetWidgetFromName",
    "/Script/UMG.UserWidget.GetWidgetFromName",
    "UserWidget:GetWidgetFromName",
    "UserWidget.GetWidgetFromName",
    "GetWidgetFromName",
]

OUT = unreal.Paths.project_saved_dir() + "gate_compass_root.txt"
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


def pins(n):
    try:
        return list(BEL.list_all_pins(n))
    except Exception:
        return []


def find_pin(node, name):
    for p in pins(node):
        if pin_name(p) == name:
            return p
    return None


def conns(p):
    try:
        return [pin_name(o) for o in BGP.list_connected_pins(p)]
    except Exception:
        return []


def wire(desc, a, b):
    if a is None or b is None:
        log("    ✗ %s：引脚找不到（a=%s b=%s）" % (desc, a, b))
        return False
    try:
        BGP.try_create_connection(a, b)
        if conns(b):           # try_create_connection 可能静默失败，必须回读
            log("    ✓ %s" % desc)
            return True
        log("    ✗ %s：调用没报错但连线没出现" % desc)
    except Exception as exc:
        log("    ✗ %s：%s" % (desc, exc))
    return False


def main():
    bp = unreal.EditorAssetLibrary.load_asset(TARGET)
    if not bp:
        log("加载失败：%s" % TARGET)
        return

    editor = BGE.get_graph_editor_by_name(bp, GRAPH)
    nodes = list(BGE.list_all_nodes(editor))
    log("图里现有 %d 个节点" % len(nodes))

    # ------------------------------------------------- 0. 清掉上一版留下的垃圾节点
    log()
    log("=" * 70)
    log("0. 清掉没有引脚的 CallFunction 节点（函数没解析上的垃圾）")
    log("=" * 70)
    junk = [n for n in nodes
            if n.get_class().get_name() == "K2Node_CallFunction" and not pins(n)]
    if junk:
        log("找到 %d 个：%s" % (len(junk), [n.get_name() for n in junk]))
        try:
            editor.remove_nodes(junk)
            log("已删除——留着会让编译报 Could not find a function named")
        except Exception as exc:
            log("删除失败：%s（手工删掉这几个节点）" % exc)
    else:
        log("没有，干净。")

    nodes = list(BGE.list_all_nodes(editor))
    log("清理后 %d 个节点（上一轮之后的基准是 35）" % len(nodes))

    # ------------------------------------------------- 锚点
    select = next((n for n in nodes if n.get_class().get_name() == "K2Node_Select"), None)
    select_ret = find_pin(select, "ReturnValue") if select else None
    if not select_ret:
        log("找不到「选择」节点或它的 ReturnValue，中止。")
        return

    # 只收集 SetVisibility；**一个 Get 都不碰**（见文件头第 3 条）
    old_sets = []
    for n in nodes:
        if "SetVisibility" not in node_title(n):
            continue
        sp = find_pin(n, "self")
        tgt = ", ".join(conns(sp)) if sp else ""
        if any(name in tgt for name in OLD_GATED):
            old_sets.append(n)
    log("旧的 SetVisibility %d 个：%s" % (len(old_sets), [n.get_name() for n in old_sets]))
    if not old_sets:
        log("一个都没找到——图可能已经改过了，先跑 compile_compass.py 看现状，中止。")
        return

    # 执行链上游
    head_src = None
    exe = find_pin(old_sets[0], "execute")
    try:
        ups = BGP.list_connected_pins(exe) if exe else []
        if ups:
            head_src = ups[0]
            log("执行链上游 = %s.%s"
                % (node_title(BGP.get_owning_node(head_src)), pin_name(head_src)))
    except Exception as exc:
        log("取上游失败：%s" % exc)
    if head_src is None:
        log("找不到执行链上游，中止——接不上执行线新节点压根不会跑。")
        return

    # ------------------------------------------------- 1. 取根面板引用
    log()
    log("=" * 70)
    log("1. GetWidgetFromName 节点（bIsVariable 是 protected，暴露成变量这条路是死的）")
    log("=" * 70)

    src_node = src_pin = None

    # 先试"它已经是变量"——这条是实测可用的（那五个 Compass_* 就是这么接的）。
    # bIsVariable 只有 Designer 界面能勾，Python 写不了（protected，已实测），
    # 所以这一步成不成取决于人有没有在界面上勾过。
    # **带对照组**：同一个调用先拿已知是变量的 Compass_N 跑一遍。
    # 这是分清"CanvasPanel_19 没勾 Is Variable"和"这个 API 对面板类型不管用"
    # 的唯一办法——只试目标的话，返回 None 这两种原因长得一模一样。
    for name, role in ((OLD_GATED[0], "对照组（已知是变量）"), (ROOT_WIDGET, "目标")):
        try:
            g = editor.add_get_member_variable_node(name)
        except Exception as exc:
            log("  %-16s %s add_get_member_variable_node 抛异常：%s" % (name, role, exc))
            continue

        if not g:
            # 返回 None 也要打出来。上一版这里什么都不打，报告里整段消失，
            # 看着像"这段代码没跑"——静默失败伪装成没执行，最难查。
            log("  %-16s %s 返回 None" % (name, role))
            continue

        gp = find_pin(g, name)
        if gp:
            log("  %-16s %s ✓ 节点建出来且引脚在" % (name, role))
            if name == ROOT_WIDGET:
                src_node, src_pin = g, gp
                continue
        else:
            log("  %-16s %s 节点建了但没有同名引脚（变量不存在）" % (name, role))
        # 对照组和失败的目标节点都不留在图里
        try:
            editor.remove_nodes([g])
        except Exception:
            pass

    # ---- 方案 C：从 Compass_N 往上调三次 GetParent 爬到根面板 ----
    # 为什么需要这条：bIsVariable 是 protected、Python 写不了（实测
    # `Property 'bIsVariable' ... is protected and cannot be set`），
    # 而 CanvasPanel_19 默认没勾 Is Variable，所以拿不到 Get 节点；
    # GetWidgetFromName 五种路径也全返回 None。
    # 但 Compass_N **是**变量，而 UWidget::GetParent() 是 BlueprintCallable，
    # 调三次正好到根：Compass_N → CanvasPanel_97 → SizeBox_20 → CanvasPanel_19。
    #
    # **代价（必须记住）**：这条链把"三层嵌套"这个事实写死了。以后谁在 Designer 里
    # 给罗盘加一层容器或改层级，链子会指到错的控件、或者拿到 null
    # （运行时 Accessed None，表现是罗盘一直显示）。
    # 勾一下 Is Variable 是一次性的、结构变了也不受影响，那条更好——
    # 这里只是为了不依赖手点。层级一旦改了，就该回去走变量那条。
    if src_pin is None:
        log()
        log("  方案 C：Get Compass_N → GetParent ×3 → 根面板")
        chain_nodes = []
        cur_pin = None
        try:
            g = editor.add_get_member_variable_node(OLD_GATED[0])
            cur_pin = find_pin(g, OLD_GATED[0]) if g else None
            if not cur_pin:
                log("    拿不到 Get %s，方案 C 也走不通" % OLD_GATED[0])
            else:
                chain_nodes.append(g)
                log("    ✓ Get %s" % OLD_GATED[0])
        except Exception as exc:
            log("    Get %s 失败：%s" % (OLD_GATED[0], exc))

        for level, expect in enumerate(("CanvasPanel_97", "SizeBox_20", ROOT_WIDGET), 1):
            if cur_pin is None:
                break
            try:
                gp = editor.add_call_function_node("GetParent")
            except Exception as exc:
                log("    第 %d 次 GetParent 建不出来：%s" % (level, exc))
                cur_pin = None
                break
            self_pin = find_pin(gp, "self")
            ret_pin = find_pin(gp, "ReturnValue")
            if not (gp and self_pin and ret_pin):
                log("    第 %d 次 GetParent 引脚不全（函数没解析上）" % level)
                if gp:
                    try:
                        editor.remove_nodes([gp])
                    except Exception:
                        pass
                cur_pin = None
                break
            chain_nodes.append(gp)
            if not wire("    GetParent #%d（应到 %s）" % (level, expect), cur_pin, self_pin):
                cur_pin = None
                break
            cur_pin = ret_pin

        if cur_pin is not None:
            src_node, src_pin = chain_nodes[-1], cur_pin
            log("    ✓ 链接好了，源引脚 = 第三个 GetParent 的 ReturnValue")
        elif chain_nodes:
            log("    方案 C 失败，清掉建出来的 %d 个节点" % len(chain_nodes))
            try:
                editor.remove_nodes(chain_nodes)
            except Exception as exc:
                log("    清理失败：%s（手工删）" % exc)

    if src_pin is None:
        log()
        log("  → 目标拿不到。看上面对照组：")
        log("    对照组成功 + 目标失败 = CanvasPanel_19 还不是变量"
            "（Is Variable 没勾，或勾了没编译保存）")
        log("    两个都失败       = 这次会话里这个 API 用不了，和勾没勾无关")

    for path in ([] if src_pin else GET_WIDGET_PATHS):
        try:
            n = editor.add_call_function_node(path)
        except Exception as exc:
            log("  %-48s 调用失败：%s" % (path, exc))
            continue
        if not n:
            log("  %-48s 返回空" % path)
            continue
        ret = find_pin(n, "ReturnValue")
        nm = find_pin(n, "Name")
        if ret and nm:
            log("  %-48s ✓ 引脚齐（Name / ReturnValue）" % path)
            src_node, src_pin = n, ret
            try:
                BGP.set_pin_value(nm, ROOT_WIDGET)
                log("    Name = %s" % BGP.get_pin_value(nm))
            except Exception as exc:
                log("    写 Name 失败：%s —— 中止" % exc)
                editor.remove_nodes([n])
                return
            break
        # 没引脚 = 没解析上，立刻删掉别留垃圾
        log("  %-48s 引脚不全（Name=%s ReturnValue=%s），删掉" % (path, nm, ret))
        try:
            editor.remove_nodes([n])
        except Exception:
            pass

    if src_pin is None:
        log()
        log("几种路径全试过都不行。下一步该怎么查：")
        log("  * 在编辑器里手动拖一个 GetWidgetFromName 节点出来，再跑 dump_bp_graph.py")
        log("    看它的真实函数路径长什么样，照抄进 GET_WIDGET_PATHS。")
        log("  * 或者放弃根面板，退回「给 Image_138 单独接一个」——但它也不是变量，")
        log("    同样要 GetWidgetFromName，绕不开。")
        log("  * **推荐**：在编辑器 Designer 里勾上 CanvasPanel_19 的 Is Variable，")
        log("    然后重跑本脚本——本脚本开头就会先试这条，而且它是实测可用的。")
        log("    bIsVariable 是 protected，Python 写不了，这一下只能人在界面上点。")
        return

    # ------------------------------------------------- 2. 新的 SetVisibility
    log()
    log("=" * 70)
    log("2. 接新的 SetVisibility（目标 = 根面板）")
    log("=" * 70)
    try:
        new_set = editor.add_call_function_node("SetVisibility")
    except Exception as exc:
        log("建不出来：%s —— 中止，什么都没删。" % exc)
        editor.remove_nodes([src_node])
        return
    if not find_pin(new_set, "InVisibility"):
        log("新节点没有 InVisibility 引脚（函数没解析上），中止并清理。")
        editor.remove_nodes([src_node, new_set])
        return

    ok = True
    ok &= wire("根面板 -> self", src_pin, find_pin(new_set, "self"))
    ok &= wire("Select -> InVisibility", select_ret, find_pin(new_set, "InVisibility"))
    # exec 输出只能连一处：接上这根，旧链自动断开，后面的删除只是清理
    ok &= wire("上游 -> execute", head_src, find_pin(new_set, "execute"))

    if not ok:
        log()
        log("接线没全成——**不删旧节点**。新建的这两个请手工删：")
        for n in (src_node, new_set):
            log("  %s  %s" % (n.get_name(), node_title(n)))
        return

    # ------------------------------------------------- 3. 删旧的五个 SetVisibility
    log()
    log("=" * 70)
    log("3. 删掉那五个 SetVisibility（Get 节点一个都不动）")
    log("=" * 70)
    log("要删：%s" % [n.get_name() for n in old_sets])
    try:
        editor.remove_nodes(old_sets)
        log("remove_nodes 完成。留下的 Get 节点变成孤立的纯数据读，编译无害。")
    except Exception as exc:
        log("失败：%s" % exc)
        log("不致命：新节点已生效、旧链已断开，这五个只是躺在图里不执行。")

    # ------------------------------------------------- 4. 编译存盘
    log()
    log("=" * 70)
    log("4. 编译 + 存盘")
    log("=" * 70)
    log("现在 %d 个节点" % len(list(BGE.list_all_nodes(editor))))
    try:
        BEL.compile_blueprint(bp)
        status = bp.get_editor_property("status")
        log("编译状态 = %s" % status)
        if "ERROR" in str(status).upper():
            log("**报错了**——去编辑器里打开 WBP_Compass 看编译器输出，它会点名哪个节点。")
    except Exception as exc:
        log("编译失败：%s" % exc)
    try:
        bp.modify(True)
        log("存盘 = %s" % unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False))
    except Exception as exc:
        log("存盘失败：%s" % exc)

    log()
    log("验收：不接电话 → 罗盘条**整条都不出现（那条线也没了）**；")
    log("      接通任务电话 → 整条出现并指向取件点；取到货 → 转为指向收件点。")


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
