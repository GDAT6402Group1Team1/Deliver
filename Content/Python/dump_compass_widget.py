# -*- coding: utf-8 -*-
"""把 WBP_Compass 的控件树、属性绑定和图表连线导出成文本。

目的：顶部罗盘条一开局就在，要改成"接到任务后才出现"。
判断条件现成就有——GetTrackedTaskCompass 返回的 bHasTarget：
Locked/Completed 为 false，AwaitingPickup（指取件点）/InProgress（指收件点）为 true。
缺的是知道这条罗盘现在**由哪个控件当根、它的 Visibility 现在绑在什么上**，
不然改绑定只能靠猜，很容易接错引脚。

和 dump_bp_graph.py 同一个路子：签名未知的 API 先打 __doc__ 再试几种调用形状，
就算全挂也把签名留在输出里，下一轮能直接对着写。

只读，不改任何资产。
用法：py dump_compass_widget.py
"""

import traceback

import unreal

TARGET = "/Game/UI/Minimap/WBP_Compass"

OUT = unreal.Paths.project_saved_dir() + "compass_widget.txt"
lines = []


def log(s=""):
    lines.append(str(s))


def try_get(obj, name):
    """读属性，失败就把原因记下来而不是中断整份导出。"""
    try:
        return True, obj.get_editor_property(name)
    except Exception as exc:
        return False, "<读不到 %s: %s>" % (name, exc)


def describe(obj):
    if obj is None:
        return "None"
    try:
        return "%s (%s)" % (obj.get_name(), obj.get_class().get_name())
    except Exception:
        return repr(obj)


# ---------------------------------------------------------------- 控件树
def dump_widget_tree(bp):
    log("=" * 70)
    log("控件树")
    log("=" * 70)

    ok, tree = try_get(bp, "WidgetTree")
    if not ok:
        log(tree)
        log("WidgetTree 读不到，改从 CDO 的 dir() 里找线索：")
        log(sorted(d for d in dir(bp) if "widget" in d.lower() or "tree" in d.lower()))
        return

    log("WidgetTree = %s" % describe(tree))

    ok, root = try_get(tree, "RootWidget")
    log("RootWidget = %s" % (describe(root) if ok else root))

    # 全部控件：WidgetTree 没有暴露遍历函数，用 get_all_widgets 之类的试，
    # 不行就退回枚举这个 package 里所有 Widget 对象。
    widgets = []
    for fn in ("get_all_widgets",):
        if hasattr(tree, fn):
            try:
                widgets = list(getattr(tree, fn)())
                log("用 %s() 取到 %d 个控件" % (fn, len(widgets)))
                break
            except Exception as exc:
                log("%s() 调用失败: %s" % (fn, exc))

    if not widgets:
        log("没有可用的遍历函数，改扫这个 package 下的 Widget 对象：")
        try:
            pkg = tree.get_outer()
            widgets = [o for o in unreal.find_all_objects_of_class(unreal.Widget)
                       if o.get_outer() == tree or o.get_outermost() == bp.get_outermost()] \
                if hasattr(unreal, "find_all_objects_of_class") else []
            log("扫到 %d 个（outer=%s）" % (len(widgets), describe(pkg)))
        except Exception as exc:
            log("扫描失败: %s" % exc)
            log("unreal 模块里和查找对象有关的函数：")
            log(sorted(d for d in dir(unreal) if "find" in d.lower() and "object" in d.lower()))

    for w in widgets:
        log("")
        log("  - %s" % describe(w))
        for prop in ("Visibility", "bIsVariable", "Slot", "RenderTransform"):
            ok, val = try_get(w, prop)
            if ok:
                log("      %s = %s" % (prop, val))

    log("")
    log("提示：根控件就是上面 RootWidget 那一行；要改的是它的 Visibility。")


# ---------------------------------------------------------------- 属性绑定
def dump_bindings(bp):
    log()
    log("=" * 70)
    log("属性绑定（Visibility 绑在哪个函数上就在这里）")
    log("=" * 70)

    ok, bindings = try_get(bp, "Bindings")
    if not ok:
        log(bindings)
        log("Bindings 读不到。蓝图上和绑定有关的属性名：")
        log(sorted(d for d in dir(bp) if "bind" in d.lower()))
        return

    try:
        bindings = list(bindings)
    except Exception as exc:
        log("Bindings 不可迭代: %s" % exc)
        return

    if not bindings:
        log("（空）——说明没有用 UMG 的属性绑定，可能是在 Tick/事件图里直接 SetVisibility。")
        log("那就去下面的图表部分找 SetVisibility 节点。")
        return

    for b in bindings:
        log("")
        for prop in ("ObjectName", "PropertyName", "FunctionName", "SourceProperty", "Kind"):
            ok, val = try_get(b, prop)
            log("  %s = %s" % (prop, val if ok else val))


# ---------------------------------------------------------------- 图表
def dump_graphs(bp):
    log()
    log("=" * 70)
    log("图表与连线")
    log("=" * 70)

    BEL = unreal.BlueprintEditorLibrary
    try:
        graphs = BEL.list_graphs(bp)
    except Exception as exc:
        log("list_graphs 失败: %s" % exc)
        log("BlueprintEditorLibrary.list_graphs.__doc__ = %s"
            % getattr(BEL.list_graphs, "__doc__", "<无>"))
        return

    log("共 %d 个图：" % len(graphs))
    for g in graphs:
        try:
            log("  - %s" % g.get_name())
        except Exception:
            log("  - %r" % g)

    for g in graphs:
        try:
            gname = str(g.get_name())
        except Exception:
            continue

        log()
        log("-" * 70)
        log("图：%s" % gname)
        log("-" * 70)

        nodes = []
        try:
            editor = unreal.BlueprintGraphEditor.get_graph_editor_by_name(bp, gname)
            nodes = list(unreal.BlueprintGraphEditor.list_all_nodes(editor))
        except Exception as exc:
            log("取节点失败: %s" % exc)
            continue

        log("%d 个节点" % len(nodes))
        for n in nodes:
            try:
                title = BEL.get_node_title(n)
            except Exception as exc:
                title = "<标题取不到: %s>" % exc
            log("")
            log("  [%s] %s" % (describe(n), title))

            try:
                pins = list(BEL.list_all_pins(n))
            except Exception as exc:
                log("      引脚取不到: %s" % exc)
                continue

            for p in pins:
                try:
                    # get_pin_name() 返回 unreal.Name 不是 str，直接切片会抛
                    # "'Name' object is not subscriptable"，整个导出断在第一个节点。
                    pname = str(unreal.BlueprintGraphPin.get_pin_name(p))
                except Exception as exc:
                    pname = "<名字取不到: %s>" % exc

                conn = []
                try:
                    for other in unreal.BlueprintGraphPin.list_connected_pins(p):
                        owner = unreal.BlueprintGraphPin.get_owning_node(other)
                        oname = str(unreal.BlueprintGraphPin.get_pin_name(other))
                        try:
                            otitle = BEL.get_node_title(owner)
                        except Exception:
                            otitle = describe(owner)
                        conn.append("%s.%s" % (otitle, oname))
                except Exception as exc:
                    conn.append("<连线取不到: %s>" % exc)

                val = ""
                try:
                    v = unreal.BlueprintGraphPin.get_pin_value(p)
                    if v not in (None, ""):
                        val = " = %s" % v
                except Exception:
                    pass

                log("      %s%s%s" % (pname, val, (" -> " + ", ".join(conn)) if conn else ""))


def main():
    log("WBP_Compass 导出")
    log("目标资产：%s" % TARGET)
    log()

    bp = unreal.EditorAssetLibrary.load_asset(TARGET)
    if not bp:
        log("加载失败——路径对不上？列一下目录：")
        try:
            for a in unreal.EditorAssetLibrary.list_assets("/Game/UI/Minimap", recursive=True):
                log("  %s" % a)
        except Exception as exc:
            log("  列目录也失败: %s" % exc)
        return

    log("资产类型：%s" % bp.get_class().get_name())

    for fn in (dump_widget_tree, dump_bindings, dump_graphs):
        try:
            fn(bp)
        except Exception:
            log()
            log("### %s 整段抛异常：" % fn.__name__)
            log(traceback.format_exc())


main()

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines[:60]))
print("...")
print("完整报告：%s" % OUT)
