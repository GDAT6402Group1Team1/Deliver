# -*- coding: utf-8 -*-
"""读出 WBP_Compass 真正的控件树，找出"按任务隐藏五个元素后还剩的那条线"。

**第一版失败的原因记在这里，别再走回头路**：
`WidgetTree` 本身能用 `unreal.find_object(bp, "WidgetTree")` 拿到（绕过了
UWidgetBlueprint 上 protected 的 WidgetTree 属性），但 **`UWidgetTree.RootWidget`
同样是 protected**，`get_editor_property("RootWidget")` 一样读不到。
于是"从根往下遍历"这条路整条断掉。
（而且第一版的兜底把错误字符串当对象继续传，树只打出一行垃圾——
取不到就该停，不该把错误伪装成数据往下传。）

这一版换方向：**从已知的叶子往上爬。**
`Compass_N` 这些是蓝图变量，所以它们是 WidgetTree 下按变量名命名的子对象，
`unreal.find_object(tree, "Compass_N")` 能直接拿到；
再用 `UWidget::GetParent()`（BlueprintCallable）一层层爬到顶，
然后用 `UPanelWidget::get_children_count/get_child_at` 往下把整棵树铺开。
那条线就是树里**不在 KNOWN 名单内**的那个元素。

只读，不改任何资产。
用法：py dump_compass_tree.py
"""

import traceback

import unreal

TARGET = "/Game/UI/Minimap/WBP_Compass"
KNOWN = ["Compass_N", "Compass_S", "Compass_E", "Compass_W", "Compass_Target"]

OUT = unreal.Paths.project_saved_dir() + "compass_tree.txt"
lines = []
others = []          # 不在 KNOWN 里的控件，就是还在显示的嫌疑


def log(s=""):
    lines.append(str(s))


def prop(obj, *names):
    """按顺序试几个属性名（Python 侧蛇形 / C++ 侧驼峰都试），取不到返回 None。"""
    for n in names:
        try:
            return obj.get_editor_property(n)
        except Exception:
            continue
    return None


def describe(w):
    try:
        return "%s  [%s]" % (w.get_name(), w.get_class().get_name())
    except Exception:
        return repr(w)


def walk(w, depth, seen):
    if w is None:
        return
    try:
        key = w.get_full_name()
    except Exception:
        key = None
    if key:
        if key in seen:
            return
        seen.add(key)

    pad = "  " * depth
    try:
        name = str(w.get_name())
    except Exception:
        name = ""

    tags = []
    is_var = prop(w, "b_is_variable", "bIsVariable")
    tags.append("变量✓" if is_var is True else ("不是变量" if is_var is False else "变量?"))
    if name in KNOWN:
        tags.append("**已接可见性**")
    else:
        tags.append("**← 没接，可能就是那条线**")
        others.append((name, describe(w), is_var))

    log("%s- %s   %s" % (pad, describe(w), " / ".join(tags)))
    log("%s    Visibility = %s" % (pad, prop(w, "visibility", "Visibility")))

    # 只有面板有子节点；不是面板就没有这两个函数，直接返回。
    try:
        count = w.get_children_count()
    except Exception:
        return
    log("%s    子节点 %d 个" % (pad, count))
    for i in range(count):
        try:
            walk(w.get_child_at(i), depth + 1, seen)
        except Exception as exc:
            log("%s    子节点[%d] 取不到：%s" % (pad, i, exc))


def main():
    bp = unreal.EditorAssetLibrary.load_asset(TARGET)
    if not bp:
        log("加载失败：%s" % TARGET)
        return

    log("WBP_Compass 控件树（从叶子往上爬）")
    log()

    tree = unreal.find_object(bp, "WidgetTree")
    if not tree:
        log("find_object(bp, 'WidgetTree') 返回空，中止。")
        return
    log("WidgetTree = %s" % describe(tree))

    # 锚点：任何一个已知变量控件
    anchor = None
    for name in KNOWN:
        try:
            anchor = unreal.find_object(tree, name)
        except Exception as exc:
            log("find_object(tree, '%s') 失败：%s" % (name, exc))
            anchor = None
        if anchor:
            log("锚点 = %s" % describe(anchor))
            break

    if not anchor:
        log()
        log("一个已知控件都找不到——名字可能和变量名不一致。")
        log("WidgetTree 下的子对象按名字找不到的话，这条路也断了，")
        log("那就只能在编辑器的 Designer 面板里直接看那条线叫什么。")
        return

    # 往上爬到顶。GetParent 是 BlueprintCallable，返回 UPanelWidget*。
    log()
    log("向上爬：")
    root = anchor
    chain = [describe(root)]
    for _ in range(32):            # 防环，32 层够深了
        try:
            parent = root.get_parent()
        except Exception as exc:
            log("  get_parent() 失败：%s" % exc)
            parent = None
        if not parent:
            break
        root = parent
        chain.append(describe(root))
    for i, c in enumerate(chain):
        log("  %s%s" % ("  " * i, c))
    log("顶层 = %s" % describe(root))

    log()
    log("=" * 70)
    log("整棵树（缩进 = 层级）")
    log("=" * 70)
    walk(root, 0, set())

    log()
    log("=" * 70)
    log("没接可见性的控件（那条线就在这里）")
    log("=" * 70)
    if others:
        for name, desc, is_var in others:
            log("  %s   变量=%s" % (desc, is_var))
    else:
        log("  无——那树里只有那五个，剩下的线可能来自别处（父级 WBP_Main？）")

    log()
    log("=" * 70)
    log("怎么改，看两点")
    log("=" * 70)
    log("1. 顶层那个是不是面板（上面写着「子节点 N 个」就是）、是不是变量：")
    log("   面板 + 变量 → 最干净：只给它一个 SetVisibility，把现有五个节点删掉")
    log("   （收起父面板，所有子元素连那条线一起没；而且它不是 UserWidget 自己，")
    log("    「事件Tick」挂在 UserWidget 上，收起内部面板不会把 Tick 停掉）。")
    log("2. 面板不是变量 → 要么先把 b_is_variable 设成 True 再接它，")
    log("   要么照现在的办法给那条线单独再接一个 SetVisibility（变成 6 个）。")


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
