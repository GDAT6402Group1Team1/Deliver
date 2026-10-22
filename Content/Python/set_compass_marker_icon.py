# -*- coding: utf-8 -*-
"""把罗盘的目标图钉改成"按任务阶段换贴图"。

在编辑器控制台跑：
    py set_compass_marker_icon.py

做的事只有一件：在 `WBP_Compass.UpdateCompass` 末尾串两个节点——

    Get Tracked Task Marker Texture  ──►  Set Brush from Texture
                                            Target: Compass_Target

待取件时返回箱子图钉、进行中返回房子图钉、没目标返回 nullptr。
**不在蓝图里接 Select 选贴图**：CLAUDE.md 立过「判断只在 `GetTrackedTaskMarker`
这一处」，映射落进蓝图就有了第二处，以后加第三种图钉漏改一个的表现是
"状态变了图标没变"。顺带也绕开了"蓝图枚举要传枚举值对象、不能传裸整数"那个坑。

—— 这个脚本为什么写得这么啰嗦 ——

改蓝图图表这条路在本项目踩过一串坑（都在 CLAUDE.md 里）：
  * `add_call_function_node` 解析不上函数时**不报错**，建出来的是个一个引脚都没有
    的空节点，留在图里要等编译才报 `Could not find a function named`
  * `try_create_connection` 可能**静默失败**，接完必须回读 `list_connected_pins` 验证
  * exec 输出引脚只能连一处，所以"先接新的、验证通过再删旧的"是安全顺序

所以这里每建一个节点就查引脚、每接一根线就回读，任何一步对不上就**停下不动**
并打印现场，绝不半截改完留个坏图。先跑一遍只会导出现状（`DRY_RUN = True`）。
"""

import unreal


BP_PATH = "/Game/UI/Minimap/WBP_Compass"
GRAPH_NAME = "UpdateCompass"          # 每帧刷新罗盘的那张图
TARGET_WIDGET = "Compass_Target"      # 要换贴图的那个 Image 控件
# **按候选列表逐个试，不要只传裸名。** CLAUDE.md 记过：裸名能解析上是运气
# （SetVisibility 成了、GetWidgetFromName 五种写法全失败），解析不上时
# 建出来的是个没有引脚的空节点、要等编译才报错。完整路径优先。
FUNC_TEXTURE = [
    "/Script/Delivery.DeliveryGuidanceLibrary:GetTrackedTaskMarkerTexture",
    "GetTrackedTaskMarkerTexture",
]
FUNC_SET_BRUSH = [
    "/Script/UMG.Image:SetBrushFromTexture",
    "SetBrushFromTexture",
]

# 先只导出、不改。看完报告确认图的形状对得上，再改成 False 跑第二遍
DRY_RUN = True

OUT = unreal.Paths.project_saved_dir() + "compass_marker_icon.txt"
_lines = []


def w(text=""):
    print(text)
    _lines.append(str(text))


def _flush():
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(_lines))
    unreal.log("[罗盘图钉] 报告写到 %s" % OUT)


def pin_names(node):
    """节点的引脚名。**空列表 = 这个节点没解析上**，留在图里编译会报错。"""
    out = []
    for p in unreal.BlueprintEditorLibrary.list_all_pins(node) or []:
        try:
            out.append(str(p.get_pin_name()))   # 返回的是 unreal.Name，不是 str
        except Exception as exc:
            out.append("<读不到:%s>" % str(exc)[:40])

    return out


def find_pin(node, name):
    for p in unreal.BlueprintEditorLibrary.list_all_pins(node) or []:
        if str(p.get_pin_name()) == name:
            return p

    return None


def add_function(editor, candidates):
    """按候选路径逐个建，**以"有没有引脚"判断解析成功**，不是看返回值非空。

    解析不上时 add_call_function_node 照样返回一个节点对象，只是一个引脚都没有；
    留在图里编译才报 `Could not find a function named`。所以失败的要当场删掉。
    """
    for path in candidates:
        node = unreal.BlueprintGraphEditor.add_call_function_node(editor, path)
        if node and pin_names(node):
            w("   解析成功: %s" % path)

            return node
        if node:
            unreal.BlueprintGraphEditor.remove_nodes(editor, [node])
        w("   解析失败: %s" % path)

    return None


def find_node(nodes, title_prefix):
    for n in nodes:
        try:
            if str(unreal.BlueprintEditorLibrary.get_node_title(n)).startswith(title_prefix):
                return n
        except Exception:
            pass

    return None


def find_exec_tail(nodes):
    """执行链的尾巴 = 有 `then` 输出、但 `then` 没接任何东西的那个节点。

    不硬写成"SetVisibility"：那是当前这张图的形状，以后队友在后面再串一个节点，
    硬写的话新节点会被插在中间、把人家的链打断。按连线找是自适应的。
    """
    tails = []
    for n in nodes:
        pin = find_pin(n, "then")
        if pin and not (unreal.BlueprintGraphPin.list_connected_pins(pin) or []):
            tails.append(n)

    return tails


def dump_graph(editor, title):
    nodes = unreal.BlueprintGraphEditor.list_all_nodes(editor) or []
    w("--- %s：%d 个节点 ---" % (title, len(nodes)))
    for i, n in enumerate(nodes):
        try:
            t = unreal.BlueprintEditorLibrary.get_node_title(n)
        except Exception:
            t = "<取不到标题>"
        w("  [%2d] %-40s 引脚: %s" % (i, str(t)[:40], ", ".join(pin_names(n))[:100]))
        # 连线也要打。只看引脚名定位不出执行链的尾巴，而串节点必须接在尾巴上
        for pin in unreal.BlueprintEditorLibrary.list_all_pins(n) or []:
            links = unreal.BlueprintGraphPin.list_connected_pins(pin) or []
            if not links:
                continue
            tgt = []
            for lp in links:
                owner = unreal.BlueprintGraphPin.get_owning_node(lp)
                try:
                    ot = str(unreal.BlueprintEditorLibrary.get_node_title(owner))[:26]
                except Exception:
                    ot = "?"
                tgt.append("%s.%s" % (ot, lp.get_pin_name()))
            w("        %-18s -> %s" % (str(pin.get_pin_name()), ", ".join(tgt)[:96]))

    return nodes


def run():
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    if not bp:
        w("!! 载不到 %s" % BP_PATH)
        _flush()
        return

    graphs = unreal.BlueprintEditorLibrary.list_graphs(bp) or []
    w("蓝图 %s 有 %d 张图表" % (BP_PATH, len(graphs)))
    graph = unreal.BlueprintEditorLibrary.find_graph(bp, GRAPH_NAME)
    if not graph:
        w("!! 找不到图表 %s。上面列出的图表名里挑一个改 GRAPH_NAME" % GRAPH_NAME)
        _flush()
        return

    editor = unreal.BlueprintGraphEditor.get_graph_editor_by_name(bp, GRAPH_NAME)
    if not editor:
        w("!! 拿不到 %s 的图表编辑器" % GRAPH_NAME)
        _flush()
        return

    dump_graph(editor, GRAPH_NAME)

    if DRY_RUN:
        w("")
        w("DRY_RUN=True，只导出没有改动。")
        w("确认上面的图里有 UpdateCompass 的主执行链之后，把脚本顶部 DRY_RUN 改成 False 再跑。")
        _flush()
        return

    nodes = unreal.BlueprintGraphEditor.list_all_nodes(editor) or []

    # —— 1. 控件引用：**复用图里已有的那个 Get Compass_Target** ——
    # 数据输出引脚可以接多处，不需要再建一个。同一个变量在一张图里出现两个 Get
    # 节点，以后按标题删节点时会误删（CLAUDE.md 记过这个坑）
    widget_node = find_node(nodes, "Get %s" % TARGET_WIDGET)
    if widget_node:
        w("复用已有的 Get %s" % TARGET_WIDGET)
    else:
        widget_node = unreal.BlueprintGraphEditor.add_get_member_variable_node(
            editor, TARGET_WIDGET)
        if not widget_node or not pin_names(widget_node):
            w("!! 图里没有、也建不出 Get %s（返回 %s）" % (TARGET_WIDGET, widget_node))
            w("   多半是这个控件没勾 Is Variable——那个复选框只能在 Designer 里点，"
              "勾完还要编译一次变量才进类（bIsVariable 是 protected，脚本写不了）")
            _flush()
            return
        w("新建 Get %s" % TARGET_WIDGET)

    # —— 2. 执行链的尾巴 ——
    tails = find_exec_tail(nodes)
    w("执行链尾巴候选 %d 个: %s" % (len(tails), [
        str(unreal.BlueprintEditorLibrary.get_node_title(t))[:30] for t in tails]))
    if len(tails) != 1:
        w("!! 尾巴不唯一，停住不动。")
        w("   多个 = 图里有并行的执行链，脚本不猜该串在哪条；0 个 = 链是闭合的。")
        w("   看上面的连线导出确认形状，再决定接哪一个。")
        _flush()
        return
    tail = tails[0]

    # —— 3. 取贴图的 C++ 函数 ——
    tex_node = add_function(editor, FUNC_TEXTURE)
    if not tex_node:
        w("!! %s 全部候选路径都没解析上。" % FUNC_TEXTURE[0])
        w("   检查：C++ 编译了吗？GetTrackedTaskMarkerTexture 是新加的，没重编就搜不到。")
        _flush()
        return
    w("建出取贴图节点，引脚: %s" % pin_names(tex_node))

    # —— 4. SetBrushFromTexture ——
    set_node = add_function(editor, FUNC_SET_BRUSH)
    if not set_node:
        w("!! SetBrushFromTexture 全部候选路径都没解析上，清理后退出。")
        unreal.BlueprintGraphEditor.remove_nodes(editor, [tex_node])
        _flush()
        return
    w("建出 SetBrushFromTexture，引脚: %s" % pin_names(set_node))

    # —— 5. 接线。每接一根立刻回读验证，try_create_connection 会静默失败 ——
    def connect(a_node, a_pin, b_node, b_pin, label):
        pa, pb = find_pin(a_node, a_pin), find_pin(b_node, b_pin)
        if not pa or not pb:
            w("   %s: 找不到引脚（%s=%s, %s=%s）" % (label, a_pin, pa, b_pin, pb))
            return False
        unreal.BlueprintGraphPin.try_create_connection(pa, pb)
        ok = any(unreal.BlueprintGraphPin.get_owning_node(p) == b_node
                 and str(p.get_pin_name()) == b_pin
                 for p in unreal.BlueprintGraphPin.list_connected_pins(pa) or [])
        w("   %-34s %s" % (label, "OK" if ok else "<< 没接上"))

        return ok

    good = True
    # exec 输出只能连一处，所以"接新的"天然会断开旧的——这里尾巴的 then 本来就是空的
    good &= connect(tail, "then", set_node, "execute", "链尾.then -> SetBrush.execute")
    good &= connect(widget_node, TARGET_WIDGET, set_node, "self", "Compass_Target -> SetBrush.self")
    good &= connect(tex_node, "ReturnValue", set_node, "Texture", "贴图 -> SetBrush.Texture")

    if not good:
        w("!! 有线没接上，已停住。新节点留在图里了——手工接完，或者删掉这两个节点重跑。")
        _flush()
        return

    # —— 5. 存盘 + 编译。状态不是 BS_UP_TO_DATE 就是接错了线 ——
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    unreal.EditorAssetLibrary.save_asset(BP_PATH)
    status = bp.get_editor_property("status")
    w("")
    w("编译状态: %s %s" % (status, "（干净）" if "UP_TO_DATE" in str(status) else "<< 有问题"))
    w("")
    w("新节点堆在图表原点附近（add_* 系列都不收坐标），连线是对的、位置难看，")
    w("在 Designer 里拖开即可。")
    _flush()


if __name__ == "__main__":
    run()
