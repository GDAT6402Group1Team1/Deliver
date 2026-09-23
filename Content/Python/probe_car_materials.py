# -*- coding: utf-8 -*-
"""查 /Game/Vehicle 下各车型的材质：混合模式、参数清单、是不是材质实例。

为什么要先查：淡入淡出要靠"把透明度参数从 1 推到 0"，而这件事成立需要两个前提
    1. 混合模式是 Translucent（或 Masked + DitherTemporalAA）
    2. 材质里有一个标量参数接到 Opacity / Opacity Mask 上
两条缺一条，SetScalarParameterValueOnMaterials 就是**静默无效**——
不报错、不抛异常，车照样不透明。之前给车上色 10 辆全"没上成"就是这个原因，
所以这次先量清楚再写代码。

输出里要看的：
    混合模式   Opaque 就得改材质，光写参数没用
    标量参数   有没有名字像 opacity / alpha / fade 的
    材质类别   MaterialInstanceConstant 的话，参数可能在父材质上

只读，什么都不改。
用法：py probe_car_materials.py
"""

import traceback

import unreal

VEHICLE_DIR = "/Game/Vehicle"
NAME_PREFIX = "BP_car_base"
OPACITY_HINTS = ["opacity", "alpha", "fade", "transparen", "dissolve"]

OUT = unreal.Paths.project_saved_dir() + "car_materials.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def blend_mode(mat):
    """取混合模式。材质实例要回溯到父材质才读得到。"""
    obj = mat
    for _ in range(6):
        try:
            bm = obj.get_editor_property("blend_mode")
            return str(bm).split(".")[-1]
        except Exception:
            pass
        try:
            parent = obj.get_editor_property("parent")
        except Exception:
            parent = None
        if parent is None:
            break
        obj = parent
    return "读不到"


def param_names(mat):
    """标量 / 向量参数名。优先用材质自带的接口，取不到就退回父材质。"""
    scal, vec = [], []
    obj = mat
    for _ in range(6):
        try:
            for p in unreal.MaterialEditingLibrary.get_scalar_parameter_names(obj):
                s = str(p)
                if s not in scal:
                    scal.append(s)
            for p in unreal.MaterialEditingLibrary.get_vector_parameter_names(obj):
                s = str(p)
                if s not in vec:
                    vec.append(s)
            if scal or vec:
                break
        except Exception:
            pass
        try:
            parent = obj.get_editor_property("parent")
        except Exception:
            parent = None
        if parent is None:
            break
        obj = parent
    return scal, vec


def run():
    try:
        paths = unreal.EditorAssetLibrary.list_assets(
            VEHICLE_DIR, recursive=False, include_folder=False)
    except Exception as exc:
        w("!! 列不出 %s：%s" % (VEHICLE_DIR, str(exc)[:60]))
        flush()
        return

    # 蓝图 CDO 上看不到 SCS 建的组件（实测 6 种车型全都读不到），
    # 所以改从**关卡里的实例**查——每个类取一辆做代表。
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    variants = []
    seen_cls = set()
    for a in eas.get_all_level_actors():
        cn = str(a.get_class().get_name())
        if not cn.startswith(NAME_PREFIX) or cn in seen_cls:
            continue
        seen_cls.add(cn)
        variants.append((cn, a))
    variants.sort(key=lambda t: t[0])
    w("关卡里出现过的车型 %d 种（每种取一辆实例做代表）" % len(variants))
    if not variants:
        w("!! 关卡里一辆车都没有，先跑 spawn_test_cars.py。")
        flush()
        return
    w("")

    # 蓝图上的网格组件要从 CDO 上取
    seen_mats = {}
    for name, actor in variants:
        w("=" * 96)
        w("%s   （实例 %s）" % (name, actor.get_actor_label()))
        w("=" * 96)
        comps = []
        try:
            comps = list(actor.get_components_by_class(unreal.MeshComponent))
        except Exception as exc:
            w("  列不出网格组件：%s" % str(exc)[:60])
        if not comps:
            w("  这辆车上没有 MeshComponent。")
        for c in comps:
            try:
                mats = c.get_materials()
            except Exception:
                mats = []
            w("  组件 %-24s 材质 %d 个" % (c.get_name()[:24], len(mats)))
            for m in mats:
                if m is None:
                    w("     (空槽位)")
                    continue
                mn = m.get_name()
                if mn in seen_mats:
                    w("     %-26s 同上" % mn[:26])
                    continue
                bm = blend_mode(m)
                scal, vec = param_names(m)
                hits = [s for s in scal
                        if any(h in s.lower() for h in OPACITY_HINTS)]
                seen_mats[mn] = (bm, scal, vec, hits)
                w("     %-26s %-18s 混合=%s"
                  % (mn[:26], m.get_class().get_name()[:18], bm))
                w("        标量参数 %d 个：%s"
                  % (len(scal), "、".join(scal[:8]) or "（没有）"))
                w("        向量参数 %d 个：%s"
                  % (len(vec), "、".join(vec[:6]) or "（没有）"))
                if hits:
                    w("        >>> 名字像透明度的：%s" % "、".join(hits))
        w("")
        flush()

    # --- 结论 ---
    w("=" * 96)
    w("结论")
    w("=" * 96)
    if not seen_mats:
        w("一个材质都没读到。关卡里的车实例上也没有网格组件？")
        w("那就先确认车确实放出来了（spawn_test_cars 的报告里有材质名）。")
        flush()
        return

    translucent = [k for k, v in seen_mats.items()
                   if v[0].lower().find("translucent") >= 0
                   or v[0].lower().find("masked") >= 0]
    with_param = [k for k, v in seen_mats.items() if v[3]]
    w("材质 %d 个：混合模式支持透明的 %d 个，带透明度参数的 %d 个"
      % (len(seen_mats), len(translucent), len(with_param)))
    w("")
    # 判据必须**混合模式和参数一起看**。只看参数名会被 glTF 导入器生成的
    # AlphaCutoff / AlphaMode 骗过去——那是 alpha 模式开关，不是能推的淡入值，
    # 而且材质本身是 BLEND_OPAQUE，推什么都不会有视觉变化。
    # （实测 53 个材质全中这个招，脚本第一版给出了"直接推参数就能淡入"的错结论。）
    usable = [k for k in with_param if k in translucent]
    if usable and len(usable) == len(seen_mats):
        w("每个材质都支持透明**且**有透明度参数 —— 直接在 C++ 里推参数就能淡入。")
    elif with_param and not translucent:
        w("有名字像透明度的参数，但**混合模式全是不透明**——这些参数推了没有视觉效果。")
        w("（glTF 导入的材质都带 AlphaCutoff / AlphaMode，那是 alpha 模式开关，不是淡入值。）")
        w("要真淡入，得先把材质实例的混合模式覆盖成 Translucent。")
    elif translucent and not with_param:
        w("混合模式支持透明、但没有参数可推 —— 要在材质里加一个标量参数接到 Opacity。")
    else:
        w("材质是 Opaque 且没有透明度参数 —— **光写参数不会有任何效果**。")
        w("三条路，代价递增：")
        w("  ① 不做透明淡入，改成**缩放淡入**（0.01 放大到目标缩放）。")
        w("     不碰材质、对任何模型都有效，观感是「由小变大」而不是「由透明变实」。")
        w("  ② 把这些材质改成 Masked + DitherTemporalAA(参数)，")
        w("     等于用抖动模拟半透明。比真半透明便宜、不会有排序问题，")
        w("     但要逐个材质改（上面列出来的每一个）。")
        w("  ③ 改成 Translucent 并加 Opacity 参数。最直观，")
        w("     但车是有厚度的实体，半透明会看到内部面片穿插，而且开销最大。")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[carmat] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[carmat] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
