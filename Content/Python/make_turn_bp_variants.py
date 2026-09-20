# -*- coding: utf-8 -*-
"""复制出两个路口段蓝图变体，各删掉一条用不着的转弯样条。

为什么要这样：实例上删组件存不住（加载时从蓝图 SCS 重建，实例侧没有
"这个组件不存在"这种可表达的差异，加 modify() 也没用）。但在**蓝图**上删
就是真删——SCS 是组件的定义源。现有蓝图不能直接删 SplineRight，
因为内道和次路还要用，所以复制两个变体：

    BP_TrafficLine1_IntersectionChild      原样保留（两条都有）-> 次路，左右都要
    BP_TrafficLine1_IntersectionChild_L    只有 SplineLeft      -> 主路外道，只左转
    BP_TrafficLine1_IntersectionChild_R    只有 SplineRight     -> 主路内道，只右转

之后 gen_traffic_lanes.py 按车道偏移选类生成，Details 面板里就没有多余组件了。

不动现有蓝图。变体已存在就跳过（可重复运行）。
用法：py make_turn_bp_variants.py
"""

import traceback

import unreal

SRC = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
VARIANTS = [("_L", "SplineRight"),   # 只留左转 -> 删掉 SplineRight
            ("_R", "SplineLeft")]    # 只留右转 -> 删掉 SplineLeft

OUT = unreal.Paths.project_saved_dir() + "make_turn_bp_variants.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[bpvar] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def comp_names(bp):
    """生成一个临时实例数组件——最直接可靠的读法。"""
    names = []
    try:
        eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        tmp = eas.spawn_actor_from_class(bp.generated_class(),
                                         unreal.Vector(0, 0, -300000))
        for c in tmp.get_components_by_class(unreal.SceneComponent):
            names.append(c.get_name())
        eas.destroy_actor(tmp)
    except Exception as exc:
        w("    数组件失败: %s" % str(exc)[:80])
    return names


def handle_name(sds, h):
    """尽力从 subobject 句柄问出组件名。"""
    try:
        data = sds.k2_find_subobject_data_from_handle(h)
    except Exception:
        return None
    lib = getattr(unreal, "SubobjectDataBlueprintFunctionLibrary", None)
    for src in (lib, sds, data):
        if src is None:
            continue
        for g in ("get_object", "get_variable_name", "get_display_name"):
            fn = getattr(src, g, None)
            if fn is None:
                continue
            try:
                val = fn(data) if src is not data else fn()
            except Exception:
                continue
            if val is None:
                continue
            try:
                return val.get_name()
            except Exception:
                return str(val)
    return None


def drop_component(sds, bp, victim):
    """在蓝图层删掉叫 victim 的组件。返回 (成功?, 说明)。"""
    try:
        handles = sds.k2_gather_subobject_data_for_blueprint(bp)
    except Exception as exc:
        return False, "取蓝图句柄失败: %s" % str(exc)[:80]
    if not handles:
        return False, "句柄为空"

    target = None
    seen = []
    for h in handles:
        nm = handle_name(sds, h)
        seen.append(nm or "?")
        if nm and victim in nm:
            target = h
    if target is None:
        return False, "按名字没认出 %s（句柄: %s）" % (victim, ", ".join(seen))

    last = ""
    for call, how in (
            (lambda: sds.delete_subobject(handles[0], target, bp), "(root, target, bp)"),
            (lambda: sds.delete_subobject(handles[0], target), "(root, target)")):
        try:
            call()
            return True, how
        except Exception as exc:
            last = "%s -> %s" % (how, str(exc)[:90])
    return False, last


def run():
    src_bp = unreal.EditorAssetLibrary.load_asset(SRC)
    if src_bp is None:
        w("!! 加载不到 %s" % SRC)
        flush()
        return
    w("源蓝图 %s" % SRC)
    w("  组件: %s" % ", ".join(comp_names(src_bp)))
    w("")

    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    made = []
    for suffix, victim in VARIANTS:
        dst = SRC + suffix
        w("=" * 68)
        w("%s  —— 删掉 %s" % (dst, victim))
        w("=" * 68)
        if unreal.EditorAssetLibrary.does_asset_exist(dst):
            bp = unreal.EditorAssetLibrary.load_asset(dst)
            w("  已存在，组件: %s" % ", ".join(comp_names(bp)))
            made.append(dst)
            continue

        bp = unreal.EditorAssetLibrary.duplicate_asset(SRC, dst)
        if bp is None:
            w("  !! 复制失败")
            continue
        w("  复制完成")

        ok, why = drop_component(sds, bp, victim)
        w("  删除 %s: %s  (%s)" % (victim, "成功" if ok else "失败", why))
        try:
            unreal.BlueprintEditorLibrary.compile_blueprint(bp)
            unreal.EditorAssetLibrary.save_asset(dst)
            w("  已编译并保存")
        except Exception as exc:
            w("  !! 编译/保存失败: %s" % str(exc)[:80])
        w("  复查组件: %s" % ", ".join(comp_names(bp)))
        if ok:
            made.append(dst)
        else:
            w("  >>> 没删成，这个变体没用，建议删掉资产重来")
        w("")
        flush()

    w("=" * 68)
    w("可用变体 %d 个" % len(made))
    for m in made:
        w("   %s" % m)
    w("")
    if len(made) == len(VARIANTS):
        w("两个变体都好了。下一步我改 gen_traffic_lanes.py：")
        w("  主路外道 -> _L，主路内道 -> _R，次路 -> 原蓝图（左右都要）")
        w("  然后 rebuild_traffic.py 重建一遍，Details 面板里就没有多余组件了。")
        w("提醒：三个蓝图以后要同步维护——改 Box、改 LightNumber 之类得改三处。")
    else:
        w("没全成。蓝图层删组件这条路走不通的话，就维持压零长（已验证可用）。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[bpvar] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常：")
    lines.append(err)
    flush()
