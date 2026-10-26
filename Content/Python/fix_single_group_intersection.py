# -*- coding: utf-8 -*-
"""给 BP_Intersection 的「压红」分支加一道闸：只有一个灯组时不压红。

病因（dump_bp_graph.py 导出真实连线后确认，2026-10-26）
------------------------------------------------------
`NextTrafficLight` 每 LightDuration 秒跑一次，结构是：
    序列 then_0: Find(key=CurrentTrafficLight) → 那组 StopAtInter=false   放绿
    序列 then_1: CurrentTrafficLight++
                 if (CurrentTrafficLight == Length(IntersectionLane)) 游标=0
                 Find(key=CurrentTrafficLight) → 那组 StopAtInter=true    压红
而 `IntersectionLane` 是 Map<LightNumber, 该组的段>，**键只有实际存在的组号**。

只有一个灯组时 Length=1：游标 0 放绿 → ++ 成 1 → `1==1` 成立 → 回绕成 0
→ **又是同一组，立刻压红**。绿只存在于同一帧内这两个节点之间，而车的
`PollForGreen` 是 0.3 秒轮询一次，永远观测不到那个 false，于是 StopAtInter
实际恒为 true，车停在路口前永远等不到绿灯。两个灯组时 `1 != 2`，交替正常——
所以这个 bug **只在单组路口出现**（交叉路没有交通线的"假路口"）。

修法：在压红的 ForEach 之前插 `Branch(Length(IntersectionLane) > 1)`，
false 就什么都不做。单组路口于是一直是绿灯，这对"交叉路根本没车道"的
路口正是想要的。**不动任何关卡数据**，一处改动覆盖全图所有路口。

为什么不删那四段 Inter_：删掉会在 Lane_S08 和 S09 之间留约 1900cm 没有样条的
缺口（TARGET_GAP 只有 150），车得靠 TraceForNewPath 盲接，而缺口超过 250
就容易接不上——那是把一个 bug 换成另一个。

DRY_RUN=True（默认）会**真的建节点**看 API 通不通、打印引脚，然后**删掉、不保存**，
只为了确认创建和引脚解析可行；不会接线、不编译、不存盘。
确认报告没问题再把 DRY_RUN 改 False 跑一次。
用法：py fix_single_group_intersection.py
"""

import traceback

import unreal

DRY_RUN = False
# 图里已经有闸时：False = 什么都不做（默认，安全）；
# True = 只保留一道、删掉多余的并重新接线。
CLEAN_EXTRA_GATES = False
BP_PATH = "/Game/PS2DEM/BP_Intersection"
GRAPH = "EventGraph"
MIN_GROUPS = 1          # Length > 这个值才压红

OUT = unreal.Paths.project_saved_dir() + "fix_single_group_intersection.txt"
lines = []

BEL = unreal.BlueprintEditorLibrary
BGE = getattr(unreal, "BlueprintGraphEditor", None)


def w(s=""):
    lines.append(str(s))
    unreal.log("[fixinter] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def attempt(label, *calls):
    last = u"（没有可试的调用）"
    for i, c in enumerate(calls):
        try:
            v = c()
            if v is not None:
                return v, None, i
        except Exception as exc:
            last = u"形状%d: %s" % (i + 1, str(exc)[:110])
    return None, u"%s 全部失败  %s" % (label, last), -1


def title(nd):
    for how in (lambda: nd.get_node_title(), lambda: BEL.get_node_title(nd)):
        try:
            t = how()
            if t:
                return str(t).replace("\n", " / ")
        except Exception:
            pass
    try:
        return nd.get_class().get_name()
    except Exception:
        return u"?"


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
    """按名字取引脚。两个坑都在这儿：

    1. `get_pin_name()` 返回 `unreal.Name` 不是 str，不转换直接比较/切片会出错。
    2. `get_pin_direction()` 返回的是 `EEdGraphPinDirection.EGPD_OUTPUT`，
       取尾段是 **`EGPD_OUTPUT`**——拿它 `startswith("Output")` 永远是 False，
       于是所有引脚都取不到，而表现只是"没找到那个节点"，看着像图被改过了。
       所以这里按 INPUT/OUTPUT 做子串匹配（"OUTPUT" 里不含 "INPUT"，不会误判）。
    """
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


def pin_default(nd, want):
    p = pin_named(nd, want)
    if p is None:
        return None
    try:
        return str(p.get_pin_value())
    except Exception:
        return None


def run():
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    w(u"=== 单组路口恒红修复 ===")
    w(u"模式：%s" % (u"DRY_RUN（建节点看通不通，随后删掉、不保存）" if DRY_RUN
                   else u"**写入**"))
    if bp is None:
        w(u"！加载不到 %s" % BP_PATH)
        return
    if BGE is None:
        w(u"！这台机器上没有 BlueprintGraphEditor，改不了图。")
        return

    ge, err, _ = attempt(u"取 graph editor",
                         lambda: BGE.get_graph_editor_by_name(bp, GRAPH),
                         lambda: BGE.get_graph_editor_by_name(bp, str(GRAPH)),
                         lambda: BGE.get_graph_editor(BEL.find_graph(bp, GRAPH)))
    if err:
        w(u"  " + err)
        return
    nodes, err, _ = attempt(u"list_all_nodes", lambda: ge.list_all_nodes())
    if err:
        w(u"  " + err)
        return
    w(u"EventGraph 共 %d 个节点" % len(nodes))

    # ---- 定位：Length 节点、压红的 ForEach ----
    length_node = None
    red_foreach = None
    red_setter = None
    for nd in nodes:
        t = title(nd)
        cls = nd.get_class().get_name()
        if length_node is None and cls == "K2Node_CallFunction" and t.strip().startswith("Length"):
            length_node = nd
        if cls == "K2Node_MacroInstance" and "For Each" in t:
            # 绿/红两个 ForEach 靠它 LoopBody 连到的 Set StopAtInter 的默认值区分：
            # 放绿那个是 false、压红那个是 true。只看标题分不出来。
            lb = pin_named(nd, "LoopBody", "Output")
            if lb is None:
                continue
            try:
                conns = lb.list_connected_pins()
            except Exception:
                conns = []
            for c in conns:
                try:
                    owner = c.get_owning_node()
                except Exception:
                    continue
                if owner is None or "StopAtInter" not in title(owner):
                    continue
                val = pin_default(owner, "StopAtInter")
                w(u"  找到 ForEach → %s，StopAtInter 默认值 = %s"
                  % (title(owner), val))
                if val is not None and val.lower() == "true":
                    red_foreach, red_setter = nd, owner

    if length_node is None:
        w(u"！没找到 Length 节点。")
        return
    if red_foreach is None:
        w(u"！没找到「压红」那个 ForEach（LoopBody 接到 StopAtInter=true 的那个）。")
        w(u"  下面把图里所有 ForEach 的 LoopBody 连到谁全列出来，对照着看是")
        w(u"  「定位判据不对」还是「图真的变了」——不列的话这两种长得一模一样。")
        for nd in nodes:
            if nd.get_class().get_name() != "K2Node_MacroInstance":
                continue
            t = title(nd)
            if "For Each" not in t:
                continue
            lb = pin_named(nd, "LoopBody", "Output")
            w(u"    ForEach「%s」 LoopBody 引脚：%s" % (t, u"取到了" if lb else u"**取不到**"))
            if lb is None:
                w(u"        这个节点的全部引脚：%s"
                  % u", ".join(u"%s(%s)" % (str(x.get_pin_name()),
                                            str(x.get_pin_direction()).split(".")[-1])
                               for x in pins(nd)))
                continue
            try:
                for c in lb.list_connected_pins():
                    ow = c.get_owning_node()
                    w(u"        → %s   StopAtInter 默认值 = %s"
                      % (title(ow), pin_default(ow, "StopAtInter")))
            except Exception as exc:
                w(u"        列连线时抛了 %s" % str(exc)[:70])
        return
    w(u"锚点定位成功：Length = %s   压红 ForEach = %s   setter = %s"
      % (title(length_node), title(red_foreach), title(red_setter)))

    len_out = pin_named(length_node, "ReturnValue", "Output")

    # ---- 幂等检查：图里已经有几道闸？----
    # 第一版没查这个，结果连跑两次就串了两道一模一样的闸（节点数 39→41→43）。
    # 功能上无害（同一条件判两次），但会无限累积，而且图越来越难读。
    # 闸的特征：Branch 的 Condition 来自一个比较节点，而那个比较节点的 A 接着 Length。
    gates = []
    for nd in nodes:
        if nd.get_class().get_name() != "K2Node_IfThenElse":
            continue
        cond = pin_named(nd, "Condition", "Input")
        if cond is None:
            continue
        try:
            srcs = cond.list_connected_pins()
        except Exception:
            srcs = []
        for sp_ in srcs:
            try:
                cmp_owner = sp_.get_owning_node()
            except Exception:
                continue
            if cmp_owner is None:
                continue
            a_pin = pin_named(cmp_owner, "A", "Input")
            if a_pin is None:
                continue
            try:
                a_srcs = [x.get_owning_node() for x in a_pin.list_connected_pins()]
            except Exception:
                a_srcs = []
            if any(x == length_node for x in a_srcs):
                gates.append((nd, cmp_owner))
                break
    w(u"图里已有的闸：%d 道" % len(gates))
    for br, cmp_n in gates:
        w(u"    Branch=%s  比较=%s" % (title(br), title(cmp_n)))

    if gates and not CLEAN_EXTRA_GATES:
        w(u"")
        w(u"已经装过闸了，**不重复添加**（再加只会串成一长串）。")
        if len(gates) > 1:
            w(u"！但现在有 %d 道重复的闸。把 CLEAN_EXTRA_GATES 改 True 再跑一次，"
              % len(gates))
            w(u"  会只留一道、把多余的删掉并重新接线。")
        else:
            w(u"闸只有一道，状态正常，什么都不用做。")
        return

    if gates and CLEAN_EXTRA_GATES:
        if len(gates) == 1:
            w(u"只有一道闸，没有多余的可清。")
            return
        keeper, keeper_cmp = gates[0]
        extra = gates[1:]
        w(u"")
        w(u"清理：保留第 1 道，删掉另外 %d 道" % len(extra))
        extra_nodes = set()
        for br, cmp_n in extra:
            extra_nodes.add(br)
            extra_nodes.add(cmp_n)
        # 找出"不是闸"的原始上游：它们该直接接 keeper.execute
        keep_exec = pin_named(keeper, "execute", "Input")
        keep_then = pin_named(keeper, "then", "Output")
        if keep_exec is None or keep_then is None:
            w(u"！保留的那道闸引脚不全，不动它。")
            return
        origin = []
        for br, _c in gates:
            ex = pin_named(br, "execute", "Input")
            if ex is None:
                continue
            try:
                for sp_ in ex.list_connected_pins():
                    ow = sp_.get_owning_node()
                    if ow is not None and ow not in [g[0] for g in gates]:
                        origin.append(sp_)
            except Exception:
                pass
        w(u"  原始上游执行线 %d 条：%s"
          % (len(origin), u"、".join(u"%s.%s" % (title(x.get_owning_node()),
                                                 str(x.get_pin_name())) for x in origin)))
        for sp_ in origin:
            try:
                sp_.try_create_connection(keep_exec)
            except Exception:
                pass
        try:
            keep_then.try_create_connection(red_exec)
        except Exception:
            pass
        ge.remove_nodes(list(extra_nodes))
        w(u"  已删 %d 个节点" % len(extra_nodes))
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)
        try:
            st = str(bp.get_editor_property("status"))
        except Exception:
            st = u"<读不到>"
        w(u"  编译状态：%s" % st)
        unreal.EditorAssetLibrary.save_loaded_asset(bp)
        w(u"已保存。再跑一次本脚本应该报「闸只有一道，状态正常」。")
        return
    red_exec = pin_named(red_foreach, "Exec", "Input")
    if len_out is None or red_exec is None:
        w(u"！取不到关键引脚：Length.ReturnValue=%s  ForEach.Exec=%s"
          % (len_out is not None, red_exec is not None))
        return

    # 现在有哪些执行线接进压红 ForEach——改线前必须记下来，这些要改接到新 Branch
    try:
        upstream = red_exec.list_connected_pins()
    except Exception:
        upstream = []
    w(u"  目前接进压红 ForEach.Exec 的执行线 %d 条：" % len(upstream))
    for p in upstream:
        try:
            w(u"      %s . %s" % (title(p.get_owning_node()), str(p.get_pin_name())))
        except Exception:
            w(u"      <读不到>")

    # ---- 建节点 ----
    made = []
    cmp_node, err, shape = attempt(
        u"建 Greater_IntInt",
        lambda: ge.add_call_function_node("/Script/Engine.KismetMathLibrary:Greater_IntInt"),
        lambda: ge.add_call_function_node("Greater_IntInt"))
    if err:
        w(u"！" + err)
        return
    made.append(cmp_node)
    w(u"  建出比较节点：%s（第 %d 种写法成功）" % (title(cmp_node), shape + 1))
    # **有引脚才说明函数真解析到了**；没引脚的空节点只会在编译时报错
    cmp_pins = [str(p.get_pin_name()) for p in pins(cmp_node)]
    w(u"      引脚：%s" % (u", ".join(cmp_pins) if cmp_pins else u"（一个都没有 → 没解析到）"))
    if not cmp_pins:
        ge.remove_nodes([cmp_node])
        w(u"  已删掉这个空节点。换个函数路径再试。")
        return

    # Branch 有**专用入口** `add_branch_node()`。
    # 别再走 create_node_from_name——那是按菜单「分类|名字」找的，
    # 12 种写法全返回 None（probe_graph_api.py 实测）。
    # 教训：先把方法表列出来（dir / list_available_nodes），再动手，不要猜名字。
    br_node, err, _ = attempt(u"建 Branch", lambda: ge.add_branch_node())
    if err:
        w(u"！" + err)
        ge.remove_nodes(made)
        w(u"  已回滚建出来的节点。")
        return
    made.append(br_node)
    br_pins = [str(p.get_pin_name()) for p in pins(br_node)]
    w(u"  建出 Branch：%s  引脚：%s" % (title(br_node), u", ".join(br_pins)))
    if "Condition" not in br_pins:
        w(u"！这个节点没有 Condition 引脚，不是 Branch。已回滚。")
        ge.remove_nodes(made)
        return

    # ---- 先接 A，再填 B ----
    # `>` 是 K2Node_PromotableOperator：**类型跟着连线推导**。不接任何东西时
    # 它解析成 Timespan（实测标题 "Timespan > Timespan"），这时 B 不接受整数，
    # set_pin_value 返回 False。先把 Length.ReturnValue（整数）接到 A，
    # 节点推导成 integer 之后 B 才填得进去。顺序反了就白改，而且表现和没改一样。
    cmp_a = pin_named(cmp_node, "A", "Input")
    if cmp_a is None:
        w(u"！比较节点没有 A 引脚。已回滚。")
        ge.remove_nodes(made)
        return
    try:
        a_ok = bool(len_out.try_create_connection(cmp_a))
    except Exception as exc:
        a_ok = False
        w(u"  接 Length.ReturnValue → >.A 抛了 %s" % str(exc)[:70])
    w(u"  接 Length.ReturnValue → >.A：%s   接后节点变成「%s」"
      % (u"ok" if a_ok else u"失败", title(cmp_node)))

    b_pin = pin_named(cmp_node, "B", "Input")
    setter_ok, setter_how = False, u"没有可用的 setter"
    if b_pin is not None:
        # probe 确认引脚上有 set_pin_value(value) -> bool，另两个是兜底
        for nm in ("set_pin_value", "set_default_value", "set_pin_default_value"):
            fn = getattr(b_pin, nm, None)
            if fn is None:
                continue
            try:
                r = fn(str(MIN_GROUPS))
                # set_pin_value 返回 bool：值不合法时返回 False 而不抛异常，
                # 只看"没抛异常"会把失败当成功，然后 B 保持 0、`Length > 0` 恒真。
                if r is False:
                    setter_how = u"%s 返回 False（值不被接受）" % nm
                    continue
                setter_ok, setter_how = True, nm
                break
            except Exception as exc:
                setter_how = u"%s 抛了 %s" % (nm, str(exc)[:60])
    b_back = None
    if b_pin is not None:
        try:
            b_back = str(b_pin.get_pin_value())
        except Exception:
            b_back = u"<读不到>"
    w(u"  填 >.B = %d：%s（%s）  回读 = %s"
      % (MIN_GROUPS, u"成功" if setter_ok else u"失败", setter_how, b_back))
    if b_back != str(MIN_GROUPS):
        w(u"  ！B 不是 %d。这一条不对的话整个修改等于没生效（Length > 0 恒真），"
          % MIN_GROUPS)
        w(u"    得在编辑器里手填那个 `>` 节点的 B。")

    if DRY_RUN:
        w(u"")
        w(u"DRY_RUN：创建和引脚解析都验证完了，现在删掉这两个节点、**不保存**。")
        ge.remove_nodes(made)
        w(u"已回滚。确认上面没有「！」之后，把文件顶部 DRY_RUN 改成 False 再跑一次。")
        w(u"（这次没有存盘，蓝图是脏的但内容没变，关编辑器时选不保存即可）")
        return

    # ---- 接线 ----
    # 执行输出引脚只能连一处，所以把上游接到 Branch 时旧线会自动断开——
    # 「先接新的、再验证」这个顺序因此是安全的。
    cmp_ret = pin_named(cmp_node, "ReturnValue", "Output")
    br_exec = pin_named(br_node, "execute", "Input")
    br_cond = pin_named(br_node, "Condition", "Input")
    br_then = pin_named(br_node, "then", "Output")
    missing = [n for n, p in (("ReturnValue", cmp_ret),
                              ("execute", br_exec), ("Condition", br_cond),
                              ("then", br_then)) if p is None]
    if missing:
        w(u"！这些引脚取不到：%s。已回滚。" % u", ".join(missing))
        ge.remove_nodes(made)
        return

    # Length → >.A 在上面已经接过了（必须先接才能填 B），这里不重复
    todo = [(cmp_ret, br_cond, u">.ReturnValue → Branch.Condition"),
            (br_then, red_exec, u"Branch.then → 压红ForEach.Exec")]
    for up in upstream:
        todo.append((up, br_exec, u"%s → Branch.execute" % title(up.get_owning_node())))

    w(u"")
    w(u"接线：")
    for src, dst, desc in todo:
        ok = False
        try:
            ok = bool(src.try_create_connection(dst))
        except Exception as exc:
            w(u"    %s —— 抛了 %s" % (desc, str(exc)[:70]))
            continue
        w(u"    %s —— %s" % (desc, u"ok" if ok else u"返回 False"))

    # try_create_connection 可能静默失败，必须回读验证
    w(u"")
    w(u"回读验证：")
    def connected(a, b):
        try:
            return any(str(p.get_pin_name()) == str(b.get_pin_name())
                       and p.get_owning_node() == b.get_owning_node()
                       for p in a.list_connected_pins())
        except Exception:
            return False
    checks = [(u"Length → >.A", connected(len_out, pin_named(cmp_node, "A", "Input"))),
              (u"> → Branch.Condition", connected(cmp_ret, br_cond)),
              (u"Branch.then → 压红ForEach", connected(br_then, red_exec))]
    for desc, ok in checks:
        w(u"    %-28s %s" % (desc, u"ok" if ok else u"**没接上**"))

    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    try:
        status = str(bp.get_editor_property("status"))
    except Exception:
        status = u"<读不到>"
    w(u"  编译状态：%s（BS_UP_TO_DATE 才算干净）" % status)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    w(u"已保存 %s" % BP_PATH)
    w(u"")
    if not setter_ok:
        w(u"**还差一步**：在编辑器里打开 BP_Intersection 的 EventGraph，")
        w(u"把新加的 `>` 节点的 B 引脚填成 %d，再编译保存。" % MIN_GROUPS)
        w(u"不填的话 B 是 0，`Length > 0` 恒真，等于这次修改没生效。")
    w(u"验收：进 PIE 看 I08 那个路口（四段全是灯组 0）的车还会不会停，")
    w(u"      或者跑 watch_stuck_intersection.py 看 StopAtInter 是不是一直 false。")
    w(u"      两个灯组的路口应该照旧每 %s 秒交替，别忘了一起看。" % u"LightDuration")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[fixinter] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[fixinter] 写入 %s" % OUT)
