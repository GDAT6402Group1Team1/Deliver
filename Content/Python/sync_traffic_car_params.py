# -*- coding: utf-8 -*-
"""核对/同步交通车组件的参数：C++ 默认值 vs 蓝图模板 vs 关卡实例。

为什么需要这一步
----------------
改 `UDeliveryTrafficCarComponent` 的 C++ 默认值**不一定生效**：
蓝图组件模板（`BP_car_base_C:Default__DeliveryTrafficCarComponent`）和关卡里的
车实例都可能把旧值**序列化**过了，那种情况下 C++ 默认值只对"没被覆盖过的"生效。
`TraceChannel` 那次就是这么栽的——默认值改成 ECC_Vehicle 之后避让仍然是死的，
得把模板也设过去、再把实例的覆盖清掉。

所以每次改这个组件的 C++ 默认值，都要跑一次本脚本确认有没有残留覆盖。

DRY_RUN=True 只核对不修改（默认）。
DRY_RUN=False 时：把实例上**等于旧值**的覆盖清掉（`reset_properties` 不可用时
改为直接写新值），让它们跟随 C++ 默认值。
用法：py sync_traffic_car_params.py
"""

import traceback

import unreal

DRY_RUN = False

# (属性名, 期望的新默认值)。期望值要和 C++ 头文件里的默认值一致。
WATCH = [
    ("ForwardTraceDistance", 900.0),
    ("MinFollowDistance", 350.0),
    ("ForwardTraceRadius", 80.0),
    ("MaxBlockedTime", 6.0),
    ("MaxBlockedTimeAtIntersection", 16.0),
    ("RouteLostTimeout", 3.0),
    ("StalledTimeout", 12.0),
    ("StalledTimeoutAtIntersection", 15.0),
    ("StallSpeedThreshold", 5.0),
]
CAR_PREFIX = "BP_car_base"
TOL = 0.01

OUT = unreal.Paths.project_saved_dir() + "sync_traffic_car_params.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[carparam] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def comp_class():
    return getattr(unreal, "DeliveryTrafficCarComponent", None)


def read(comp, name):
    """UE 的 Python 绑定对 float UPROPERTY 用原名就能读；读不到返回 None。"""
    for n in (name, name.lower()):
        try:
            return float(comp.get_editor_property(n))
        except Exception:
            continue
    return None


def run():
    cc = comp_class()
    w(u"=== 交通车组件参数核对 ===")
    w(u"模式：%s" % (u"只核对" if DRY_RUN else u"**写入**"))
    if cc is None:
        w(u"！unreal.DeliveryTrafficCarComponent 不存在——编辑器加载的 C++ 模块里没有它。")
        w(u"  先关编辑器、编译 DeliveryEditor、重开，再跑本脚本。")
        return

    # ---- C++ 类默认值（CDO）----
    w(u"")
    w(u"① C++ 类默认值（CDO）")
    cdo = unreal.get_default_object(cc)
    for name, want in WATCH:
        got = read(cdo, name) if cdo else None
        mark = u""
        if got is None:
            mark = u"  ← 读不到"
        elif abs(got - want) > TOL:
            mark = u"  ← **和期望的 %.1f 不一致：C++ 改了但没重编译？**" % want
        w(u"    %-32s %s%s" % (name, u"?" if got is None else u"%.1f" % got, mark))

    # ---- 关卡实例 ----
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    cars = []
    for a in eas.get_all_level_actors():
        if not a.get_class().get_name().startswith(CAR_PREFIX):
            continue
        comp = a.get_component_by_class(cc)
        if comp:
            cars.append((a, comp))
    w(u"")
    w(u"② 关卡里的车 %d 辆" % len(cars))
    if not cars:
        w(u"    这张图里没有 %s。" % CAR_PREFIX)
        return

    # 按"这辆车读出来的一整组值"归类，相同的合并，不然 32 辆车刷一屏
    groups = {}
    for a, comp in cars:
        key = tuple((name, read(comp, name)) for name, _wv in WATCH)
        groups.setdefault(key, []).append((a, comp))
    w(u"    按读出来的值分成 %d 组：" % len(groups))
    stale = []
    for key, members in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        diffs = [(n, v, wv) for (n, v), (_n2, wv) in zip(key, WATCH)
                 if v is None or abs(v - wv) > TOL]
        w(u"")
        w(u"    [%d 辆] %s" % (len(members),
                              u"  ".join(u"%s=%s" % (n, u"?" if v is None else u"%.1f" % v)
                                         for n, v in key)))
        if diffs:
            w(u"        ！和期望不一致：%s"
              % u"、".join(u"%s 应为 %.1f" % (n, wv) for n, _v, wv in diffs))
            stale.extend(members)
        else:
            w(u"        和 C++ 默认值一致，不用动")

    w(u"")
    w(u"=" * 72)
    if not stale:
        w(u"所有车都跟着 C++ 默认值走，没有残留覆盖——什么都不用做。")
        return
    w(u"有 %d 辆车带着旧值（实例覆盖或模板覆盖）。" % len(stale))
    if DRY_RUN:
        w(u"把 DRY_RUN 改 False 再跑一次，会把它们改成期望值。")
        w(u"（改完记得 Ctrl+S 保存关卡）")
        return

    # **优先清掉实例覆盖，而不是写新值。**
    # 写新值等于又冻一次：这个实例从此带着一个"恰好等于当前默认值"的覆盖，
    # 下次改 C++ 默认值它照样不跟。清掉覆盖才能让它永久跟随 CDO。
    # Python 暴露的清除 API 不确定，所以几种形状都试一遍，都不行再退回写值。
    reset_api = None
    for nm in ("reset_properties", "reset_property"):
        if getattr(stale[0][1], nm, None) is not None:
            reset_api = nm
            break
    w(u"清除覆盖的 API：%s" % (reset_api or u"没有，退回直接写值"))

    fixed, failed = 0, 0
    for a, comp in stale:
        ok = True
        names = [n for n, wv in WATCH
                 if (read(comp, n) is None or abs(read(comp, n) - wv) > TOL)]
        if not names:
            continue
        comp.modify(True)
        a.modify(True)

        if reset_api is not None:
            try:
                fn = getattr(comp, reset_api)
                if reset_api == "reset_properties":
                    fn(names)
                else:
                    for n in names:
                        fn(n)
            except Exception as exc:
                w(u"    %s 清覆盖失败（%s），改为写值：%s"
                  % (a.get_actor_label(), reset_api, str(exc)[:50]))

        # 不管走的是清除还是写值，一律**回读确认**——清除可能静默无效。
        for name, want in WATCH:
            cur = read(comp, name)
            if cur is not None and abs(cur - want) <= TOL:
                continue
            try:
                comp.set_editor_property(name, want)
                back = read(comp, name)
                if back is None or abs(back - want) > TOL:
                    ok = False
                    w(u"    %s 的 %s 写了但回读是 %s"
                      % (a.get_actor_label(), name, back))
            except Exception as exc:
                ok = False
                w(u"    %s 的 %s 写失败：%s"
                  % (a.get_actor_label(), name, str(exc)[:60]))
        if ok:
            fixed += 1
        else:
            failed += 1
    w(u"")
    w(u"写好 %d 辆，失败 %d 辆。**记得 Ctrl+S 保存关卡。**" % (fixed, failed))
    w(u"注意：第二/三波车是运行时克隆的，会继承被克隆那辆的值，所以不用单独处理。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[carparam] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[carparam] 写入 %s" % OUT)
