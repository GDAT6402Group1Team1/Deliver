# -*- coding: utf-8 -*-
"""给 BP_TrafficLine1_IntersectionChild 加两条样条组件：SplineLeft / SplineRight。

目的：路口段 actor 内部同时挂直行、左转、右转三条样条，共用同一个 Box。
车探测到那一个 Box 就拿到三条候选路径，而不是三个各自带 Box 的 actor。

加在**蓝图**上而不是实例上：这样每个实例天生就有这三条样条，
脚本只需要逐实例往里写点——和现在给 Spline 写点是同一套机制，已经验证可行。
反过来给实例动态加组件这条路早先试过：AddComponentByClass 是 ScriptNoExport，
Python 调不到（当初生成裙边就是因此退成"一个 mesh 一个 actor"）。

这里改用 UE5 的 SubobjectDataSubsystem。这个 API 在本项目没验证过，
所以失败时会把可用的接口和参数都打出来，不至于白跑一趟。

只改蓝图，不动关卡里的实例。
用法：py add_turn_splines.py
"""

import traceback

import unreal

BP_PATH = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
WANT = ["SplineLeft", "SplineRight"]

OUT = unreal.Paths.project_saved_dir() + "add_turn_splines.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[addspline] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def dump_api(obj, title):
    w("  %s 可用成员：" % title)
    for n in sorted(set(dir(obj))):
        if not n.startswith("_"):
            w("     %s" % n)


def existing_component_names(bp):
    """看蓝图现在有哪些组件——生成一个临时实例数一下最直接。"""
    names = []
    try:
        eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        tmp = eas.spawn_actor_from_class(bp.generated_class(),
                                         unreal.Vector(0, 0, -300000))
        for c in tmp.get_components_by_class(unreal.SceneComponent):
            names.append("%s (%s)" % (c.get_name(), c.get_class().get_name()))
        eas.destroy_actor(tmp)
    except Exception as exc:
        w("  数组件失败: %s" % str(exc)[:80])
    return names


def run():
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    if bp is None:
        w("!! 加载不到 %s" % BP_PATH)
        flush()
        return
    w("蓝图 %s" % BP_PATH)

    before = existing_component_names(bp)
    w("现有组件 %d 个：" % len(before))
    for n in before:
        w("   %s" % n)
    have = set(n.split(" ")[0] for n in before)
    todo = [n for n in WANT if n not in have]
    if not todo:
        w("")
        w("SplineLeft / SplineRight 都已经存在，不用再加。")
        w("下一步：跑填点脚本把左右转曲线写进去。")
        flush()
        return
    w("")
    w("要新增：%s" % ", ".join(todo))
    w("")

    sds = None
    for how in ("get_engine_subsystem", "get_editor_subsystem"):
        fn = getattr(unreal, how, None)
        cls = getattr(unreal, "SubobjectDataSubsystem", None)
        if fn is None or cls is None:
            continue
        try:
            sds = fn(cls)
            if sds is not None:
                w("拿到 SubobjectDataSubsystem（via %s）" % how)
                break
        except Exception:
            continue
    if sds is None:
        w("!! 这个引擎版本拿不到 SubobjectDataSubsystem。")
        w("   请手动加：打开 %s -> Add Component -> Spline -> 改名成 %s"
          % (BP_PATH, " / ".join(todo)))
        w("   加完再跑一次本脚本确认，然后跑填点脚本。")
        flush()
        return

    handles, err = None, None
    for how in ("k2_gather_subobject_data_for_blueprint",
                "k2_gather_subobject_data_for_instance",
                "gather_subobject_data_for_blueprint"):
        fn = getattr(sds, how, None)
        if fn is None:
            continue
        try:
            handles = fn(bp)
            w("用 %s 拿到 %d 个 subobject 句柄" % (how, len(handles)))
            break
        except Exception as exc:
            err = "%s: %s" % (how, str(exc)[:90])
    if not handles:
        w("!! 取不到 subobject 句柄。%s" % (err or ""))
        dump_api(sds, "SubobjectDataSubsystem")
        w("   先按手动方式加组件（见上），不用等这个 API。")
        flush()
        return

    root = handles[0]
    made = []
    for name in todo:
        try:
            params = unreal.AddNewSubobjectParams(
                parent_handle=root,
                new_class=unreal.SplineComponent,
                blueprint_context=bp)
        except Exception as exc:
            w("!! 构造 AddNewSubobjectParams 失败: %s" % str(exc)[:90])
            cls = getattr(unreal, "AddNewSubobjectParams", None)
            if cls is not None:
                dump_api(cls, "AddNewSubobjectParams")
            break
        try:
            res = sds.add_new_subobject(params)
            handle = res[0] if isinstance(res, tuple) else res
            sds.rename_subobject(handle, name)
            made.append(name)
            w("已加 %s" % name)
        except Exception as exc:
            w("!! 加 %s 失败: %s" % (name, str(exc)[:120]))
            dump_api(sds, "SubobjectDataSubsystem")
            break

    if made:
        try:
            unreal.BlueprintEditorLibrary.compile_blueprint(bp)
            unreal.EditorAssetLibrary.save_asset(BP_PATH)
            w("蓝图已编译并保存。")
        except Exception as exc:
            w("!! 编译/保存失败（需要手动保存蓝图）: %s" % str(exc)[:90])
        w("")
        w("复查组件清单：")
        for n in existing_component_names(bp):
            w("   %s" % n)
        w("")
        w("下一步：跑填点脚本把左右转曲线写进 %s。" % " / ".join(made))
    else:
        w("")
        w("一个都没加成。请手动加：打开蓝图 -> Add Component -> Spline -> 改名成 %s"
          % " / ".join(todo))
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[addspline] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
