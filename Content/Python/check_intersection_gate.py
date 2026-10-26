# -*- coding: utf-8 -*-
"""查 BP_Intersection 的红/绿分支现在到底怎么接的。默认只读。

背景：fix_single_group_intersection.py 的**清理路径**把 `keeper.then → 压红ForEach.Exec`
这一步只 try/except、没有回读验证（添加路径有验证，清理路径漏了）。
而 ForEach 的 Exec 悬空**不是编译错误**，所以编译状态照样是 BS_UP_TO_DATE。
症状：压红分支永不执行 → 全图绿灯、红色 DrawDebugSphere 不再出现、没有车停。

本脚本把两个分支的实际接线打出来。REPAIR=True 时按正确结构重接并逐根回读验证。

正确结构（dump_bp_graph.py 导出的原始形态 + 一道闸）：
    序列.then_0 → 放绿ForEach.Exec
    序列.then_1 → Increment Int → 分支(游标回绕)
        分支.then  → Set CurrentTrafficLight → ┐
        分支.else  →                          ├→ 闸.execute
        闸.Condition ← (Length > 1)
        闸.then    → 压红ForEach.Exec
用法：py check_intersection_gate.py
"""

import traceback

import unreal

REPAIR = False
BP_PATH = "/Game/PS2DEM/BP_Intersection"
GRAPH = "EventGraph"
MIN_GROUPS = 1

OUT = unreal.Paths.project_saved_dir() + "check_intersection_gate.txt"
lines = []
BEL = unreal.BlueprintEditorLibrary
BGE = getattr(unreal, "BlueprintGraphEditor", None)


def w(s=""):
    lines.append(str(s))
    unreal.log("[gate] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def title(nd):
    if nd is None:
        return u"<空>"
    for how in (lambda: nd.get_node_title(), lambda: BEL.get_node_title(nd)):
        try:
            t = how()
            if t:
                return str(t).replace("\n", " / ")
        except Exception:
            pass
    return nd.get_class().get_name()


def pins(nd):
    for how in (lambda: nd.list_all_pins(), lambda: BEL.list_all_pins(nd)):
        try:
            v = how()
            if v:
                return v
        except Exception:
            pass
    return []


def pin_named(nd, want, direction=None):
    # get_pin_direction() 返回 EEdGraphPinDirection.EGPD_OUTPUT，尾段是 EGPD_OUTPUT
    want_dir = direction.upper() if direction else None
    for p in pins(nd):
        try:
            nm = str(p.get_pin_name())
            d = str(p.get_pin_direction()).upper()
        except Exception:
            continue
        if nm.lower() != want.lower():
            continue
        if want_dir is None or want_dir in d:
            return p
    return None


def conns(p):
    if p is None:
        return []
    try:
        return list(p.list_connected_pins())
    except Exception:
        return []


def show(p, label):
    cs = conns(p)
    if not cs:
        w(u"    %-34s **没有任何连线**" % label)
        return []
    for c in cs:
        try:
            w(u"    %-34s ← / → %s . %s"
              % (label, title(c.get_owning_node()), str(c.get_pin_name())))
        except Exception:
            w(u"    %-34s ← / → <读不到>" % label)
    return cs


def run():
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    w(u"=== BP_Intersection 红/绿分支接线检查 ===")
    # 三元写反过一次：REPAIR=False 时打印"**修复**"，看报告的人会以为改过了。
    w(u"模式：%s" % (u"**修复**" if REPAIR else u"只读"))
    if bp is None or BGE is None:
        w(u"！加载不到蓝图或没有 BlueprintGraphEditor")
        return
    ge = None
    for how in (lambda: BGE.get_graph_editor_by_name(bp, GRAPH),
                lambda: BGE.get_graph_editor(BEL.find_graph(bp, GRAPH))):
        try:
            ge = how()
            if ge:
                break
        except Exception:
            pass
    if ge is None:
        w(u"！取不到 graph editor")
        return
    nodes = ge.list_all_nodes()
    w(u"节点 %d 个" % len(nodes))

    green_fe = red_fe = None
    length_node = None
    seq = None
    gates = []
    for nd in nodes:
        cls = nd.get_class().get_name()
        t = title(nd)
        if cls == "K2Node_CallFunction" and t.strip().startswith("Length"):
            length_node = nd
        if cls == "K2Node_ExecutionSequence":
            seq = nd
        if cls == "K2Node_MacroInstance" and "For Each" in t:
            lb = pin_named(nd, "LoopBody", "Output")
            for c in conns(lb):
                ow = c.get_owning_node()
                if ow is None or "StopAtInter" not in title(ow):
                    continue
                sp_ = pin_named(ow, "StopAtInter")
                val = None
                if sp_ is not None:
                    try:
                        val = str(sp_.get_pin_value())
                    except Exception:
                        val = None
                if val and val.lower() == "true":
                    red_fe = nd
                elif val and val.lower() == "false":
                    green_fe = nd
        if cls == "K2Node_IfThenElse":
            cond = pin_named(nd, "Condition", "Input")
            for c in conns(cond):
                cmp_n = c.get_owning_node()
                if cmp_n is None:
                    continue
                a = pin_named(cmp_n, "A", "Input")
                if a is None:
                    continue
                if any(x.get_owning_node() == length_node for x in conns(a)
                       if length_node is not None):
                    gates.append((nd, cmp_n))
                    break

    w(u"")
    w(u"定位：Length=%s  序列=%s  放绿ForEach=%s  压红ForEach=%s  闸 %d 道"
      % (u"有" if length_node else u"**无**", u"有" if seq else u"**无**",
         u"有" if green_fe else u"**无**", u"有" if red_fe else u"**无**", len(gates)))

    if seq is not None:
        w(u"")
        w(u"序列（NextTrafficLight 的两步）：")
        show(pin_named(seq, "then_0", "Output"), u"then_0（应接放绿ForEach）")
        show(pin_named(seq, "then_1", "Output"), u"then_1（应接 Increment Int）")

    for nm, fe in ((u"放绿", green_fe), (u"压红", red_fe)):
        w(u"")
        w(u"%sForEach：" % nm)
        if fe is None:
            w(u"    **没找到**")
            continue
        show(pin_named(fe, "Exec", "Input"), u"Exec（谁驱动它）")
        show(pin_named(fe, "Array", "Input"), u"Array（遍历谁）")

    for i, (br, cmp_n) in enumerate(gates):
        w(u"")
        w(u"闸 %d：Branch=%s   比较=%s" % (i + 1, title(br), title(cmp_n)))
        show(pin_named(br, "execute", "Input"), u"execute（谁驱动它）")
        show(pin_named(br, "Condition", "Input"), u"Condition")
        show(pin_named(br, "then", "Output"), u"then（条件真时走哪）")
        show(pin_named(br, "else", "Output"), u"else（条件假时走哪）")
        bp_ = pin_named(cmp_n, "B", "Input")
        try:
            w(u"    %-34s = %s" % (u">.B 字面量", str(bp_.get_pin_value()) if bp_ else u"?"))
        except Exception:
            w(u"    >.B 读不到")
        show(pin_named(cmp_n, "A", "Input"), u">.A（应接 Length）")

    w(u"")
    w(u"=" * 72)
    w(u"诊断")
    w(u"=" * 72)
    broken = []
    if red_fe is not None and not conns(pin_named(red_fe, "Exec", "Input")):
        broken.append(u"压红ForEach 的 Exec 悬空 → 永不压红 → 全图绿灯、红球不出现")
    if green_fe is not None and not conns(pin_named(green_fe, "Exec", "Input")):
        broken.append(u"放绿ForEach 的 Exec 悬空 → 永不放绿")
    if len(gates) != 1:
        broken.append(u"闸有 %d 道（应为 1 道）" % len(gates))
    for i, (br, cmp_n) in enumerate(gates):
        if not conns(pin_named(br, "execute", "Input")):
            broken.append(u"闸 %d 的 execute 悬空 → 它后面整条链都不会执行" % (i + 1))
        if not conns(pin_named(br, "then", "Output")):
            broken.append(u"闸 %d 的 then 没接出去 → 条件为真也不压红" % (i + 1))
    if broken:
        for b in broken:
            w(u"  ！%s" % b)
        w(u"")
        w(u"把 REPAIR 改 True 再跑一次，会按正确结构重接并逐根回读验证。")
    else:
        w(u"  接线看起来完整。那症状的原因在别处——把这份报告发我。")

    if not REPAIR or not broken:
        return

    # ---- 修复 ----
    if red_fe is None or seq is None or length_node is None or len(gates) != 1:
        w(u"")
        w(u"！缺少关键节点或闸不是 1 道，不敢自动修。把报告发我。")
        return
    br, cmp_n = gates[0]
    red_exec = pin_named(red_fe, "Exec", "Input")
    br_then = pin_named(br, "then", "Output")
    br_exec = pin_named(br, "execute", "Input")

    # 原始上游：序列.then_1 那条链的末端——分支(游标回绕).else 和 Set CurrentTrafficLight.then
    srcs = []
    for nd in nodes:
        t = title(nd)
        cls = nd.get_class().get_name()
        if cls == "K2Node_IfThenElse" and nd != br:
            p = pin_named(nd, "else", "Output")
            if p is not None:
                srcs.append((u"%s.else" % t, p))
        if cls == "K2Node_VariableSet" and "CurrentTrafficLight" in t:
            p = pin_named(nd, "then", "Output")
            if p is not None:
                srcs.append((u"%s.then" % t, p))
    w(u"")
    w(u"修复：把这些重新接到闸的 execute，再把闸的 then 接回压红ForEach")
    for nm, p in srcs:
        ok = False
        try:
            ok = bool(p.try_create_connection(br_exec))
        except Exception as exc:
            w(u"    %s → 闸.execute 抛了 %s" % (nm, str(exc)[:60]))
            continue
        w(u"    %s → 闸.execute：%s" % (nm, u"ok" if ok else u"返回 False"))
    try:
        ok = bool(br_then.try_create_connection(red_exec))
        w(u"    闸.then → 压红ForEach.Exec：%s" % (u"ok" if ok else u"返回 False"))
    except Exception as exc:
        w(u"    闸.then → 压红ForEach.Exec 抛了 %s" % str(exc)[:60])

    w(u"")
    w(u"回读验证（这一步上次漏了，所以才出事）：")
    bad = False
    for nm, p in (("闸.execute", br_exec), ("闸.then", br_then),
                  ("压红ForEach.Exec", red_exec)):
        c = conns(p)
        w(u"    %-20s %d 条连线" % (nm, len(c)))
        if not c:
            bad = True
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    try:
        st = str(bp.get_editor_property("status"))
    except Exception:
        st = u"<读不到>"
    w(u"  编译状态：%s" % st)
    if bad:
        w(u"！还有悬空引脚，**没有保存**。把报告发我。")
        return
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    w(u"已保存。进 PIE 确认：两组路口要有红球和停车，单组路口不停。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[gate] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[gate] 写入 %s" % OUT)
