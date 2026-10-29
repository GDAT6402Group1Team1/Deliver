# -*- coding: utf-8 -*-
u"""找出 BP_car_base 的父类，并导出父类里 SetCurentSplineComp 的实现。

为什么要查这个（2026-10-29）
----------------------------
`SetCurentSplineComp` 决定车焊到命中段的**哪一条**样条（主 / 左转 / 右转），
而 CLAUDE.md 记了很久：它不在 `BP_car_base` 自己的 7 个图里，是继承来的、
**从没查到实现**。`Inter_BranchRoad_R037_-0180_I03` 左转后几乎都裸奔，
而整条左转路线的几何已经逐段量过、全部接得上（纵向 150 左右、横向 1~4 cm），
所以剩下的静态未知项就只有这个函数本身。

**不猜它是「随机三选一」**：CLAUDE.md 里那句"选路是随机的"引用的是
`Array_Random` 的存在，而那个节点不在这几个图里，等于没有证据。

只读，不改任何东西。结果写到 Saved/dump_car_parent.txt
用法：py dump_car_parent.py
"""

import traceback

import unreal

CAR_BP = "/Game/Vehicle/BP_car_base"
WANT = ("SetCurentSplineComp", "DriveForward", "GetFrameRate")

OUT = unreal.Paths.project_saved_dir() + "dump_car_parent.txt"
lines = []


def w(s=u""):
    lines.append(str(s))
    unreal.log("[parent] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def load_bp(path):
    u"""load_asset 会把 ObjectRedirector 当成对象成功返回（实测 BP_car_base
    从 /Game/Blueprint/ 搬到了 /Game/Vehicle/，旧路径剩 2.2KB 重定向器），
    所以必须显式查类名，否则后面所有 API 都报「全部失败」、看着像 API 坏了。"""
    a = unreal.EditorAssetLibrary.load_asset(path)
    if a is None:
        w(u"!! 加载不到 %s" % path)
        return None
    cn = a.get_class().get_name()
    if cn == "ObjectRedirector":
        w(u"!! %s 是 ObjectRedirector（资产搬过位置）" % path)
        return None
    if cn not in ("Blueprint", "WidgetBlueprint"):
        w(u"!! %s 的类是 %s，不是 Blueprint" % (path, cn))
        return None
    return a


def parent_of(bp):
    u"""父类。每条路都打日志——返回 None 不打日志会让「读不到」伪装成「没执行」。"""
    for n in ("parent_class", "ParentClass"):
        try:
            v = bp.get_editor_property(n)
            if v is not None:
                return v, u"get_editor_property(%s)" % n
        except Exception as exc:
            w(u"  （%s 读不到：%s）" % (n, exc))
    try:
        gen = unreal.BlueprintEditorLibrary.generated_class(bp)
        if gen is not None:
            sup = gen.get_super_class()
            if sup is not None:
                return sup, u"generated_class().get_super_class()"
    except Exception as exc:
        w(u"  （generated_class 兜底失败：%s）" % exc)
    return None, u"全部读不到"


def dump_graphs(bp, label):
    names = [str(n) for n in unreal.BlueprintEditorLibrary.list_graph_names(bp)]
    w(u"  %s 的图（%d 个）：%s" % (label, len(names), u"、".join(names)))
    for gname in names:
        if gname not in WANT:
            continue
        w(u"")
        w(u"  " + u"=" * 70)
        w(u"  图 %s" % gname)
        w(u"  " + u"=" * 70)
        ed = unreal.BlueprintEditorLibrary.get_graph_editor_by_name(bp, gname)
        if ed is None:
            w(u"  !! 拿不到图编辑器")
            continue
        nodes = ed.list_all_nodes()
        w(u"  节点 %d 个" % len(nodes))
        for nd in nodes:
            w(u"  [%s] %s" % (nd.get_class().get_name(),
                              unreal.BlueprintEditorLibrary.get_node_title(nd)))
            for pin in unreal.BlueprintEditorLibrary.list_all_pins(nd):
                # get_pin_name() 返回 unreal.Name 不是 str，直接切片会抛
                # 'Name' object is not subscriptable、整个导出断在第一个节点。
                pn = str(pin.get_pin_name())
                val = pin.get_pin_value()
                conns = []
                for c in pin.list_connected_pins():
                    try:
                        conns.append(u"%s.%s" % (
                            unreal.BlueprintEditorLibrary.get_node_title(
                                c.get_owning_node()), str(c.get_pin_name())))
                    except Exception:
                        conns.append(u"?")
                w(u"      %-26s %-28s %s"
                  % (pn, (val or u"")[:26],
                     (u"→ " + u"；".join(conns)) if conns else u""))


def run():
    w(u"=== BP_car_base 的父类与 SetCurentSplineComp 实现（只读）===")
    bp = load_bp(CAR_BP)
    if bp is None:
        return
    dump_graphs(bp, CAR_BP)

    w(u"")
    seen, cur, depth = set(), bp, 0
    while cur is not None and depth < 8:
        par, how = parent_of(cur)
        if par is None:
            w(u"父类链到此为止（%s）" % how)
            break
        pname = par.get_name()
        ppath = par.get_path_name()
        w(u"父类：%s    （%s）    %s" % (pname, how, ppath))
        if pname in seen:
            w(u"!! 出现环，停")
            break
        seen.add(pname)

        # 父类是另一个蓝图时，资产路径是 .../X.X_C，去掉 _C 和包名即可
        if pname.endswith("_C"):
            asset = ppath.split(".")[0]
            w(u"  → 这是个蓝图，导它的图：%s" % asset)
            pbp = load_bp(asset)
            if pbp is None:
                break
            dump_graphs(pbp, asset)
            cur = pbp
            depth += 1
            continue
        w(u"  → 这是原生 C++ 类，到这里就不是蓝图了。"
          u"若 SetCurentSplineComp 仍没出现，它应该是这个类上的 C++ 函数，"
          u"去 Source 里搜，或者在编辑器里右键节点 → 转到定义。")
        break


try:
    run()
except Exception:
    w(u"!! 脚本异常：")
    w(traceback.format_exc())
flush()
w(u"报告：%s" % OUT)
