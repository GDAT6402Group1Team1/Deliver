# -*- coding: utf-8 -*-
"""探路：从单个实例上删掉蓝图定义的 SplineRight 组件。

（这是多路探测那一版，恢复回来备用。后来那版只保留了 destroy_component
  一条路加 modify()，用来验持久性。结论：当场删得掉，但**存不住**——
  组件在加载时从蓝图 SCS 重建，实例侧没有"这个组件不存在"这种可表达的差异，
  加 modify() 也救不了，因为被删的对象本身没了、没有覆盖数据可存。
  样条**点数据**的实例覆盖则是存得住的，前提是写之前调 modify()。）

这一版先把签名打出来再调，每条路都立刻回读：
  1. component.destroy_component(object)
  2. SubobjectDataSubsystem.k2_delete_subobject_from_instance
  3. 都不行的话把可用接口打出来

试验对象是一条**外道**（只左转，SplineRight 用不着），
删成功也不会破坏在用的数据。
用法：py probe_del_component.py
"""

import traceback

import unreal

TARGET = "Inter_形状13_-0540_I04"
VICTIM = "SplineRight"

OUT = unreal.Paths.project_saved_dir() + "probe_del_component.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[delprobe] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def doc(obj, name):
    fn = getattr(obj, name, None)
    if fn is None:
        return "  %-40s （没有）" % name
    d = (getattr(fn, "__doc__", "") or "").strip().replace("\n", " ")
    return "  %-40s %s" % (name, d[:170])


def splines(a):
    return [c.get_name() for c in a.get_components_by_class(unreal.SplineComponent)]


def find_victim(a):
    for c in a.get_components_by_class(unreal.SplineComponent):
        if c.get_name() == VICTIM:
            return c
    return None


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    a = None
    for x in eas.get_all_level_actors():
        if x.get_actor_label() == TARGET:
            a = x
            break
    if a is None:
        w("!! 找不到 %s" % TARGET)
        flush()
        return
    w("试验对象 %s" % TARGET)
    w("  现有样条组件: %s" % ", ".join(splines(a)))
    w("")

    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    w("=" * 70)
    w("签名")
    w("=" * 70)
    v = find_victim(a)
    if v is not None:
        w(doc(v, "destroy_component"))
    for n in ("k2_delete_subobject_from_instance",
              "k2_delete_subobjects_from_instance",
              "delete_subobject", "delete_subobjects",
              "k2_gather_subobject_data_for_instance",
              "k2_find_subobject_data_from_handle"):
        w(doc(sds, n))
    w("")
    flush()

    if v is None:
        w("%s 不在这个 actor 上，无从试起。" % VICTIM)
        flush()
        return

    # --- 路 1: k2_delete_subobject_from_instance ---
    w("=" * 70)
    w("路 1: k2_delete_subobject_from_instance（实例专用）")
    w("=" * 70)
    try:
        handles = sds.k2_gather_subobject_data_for_instance(a)
        w("  句柄 %d 个" % len(handles))
    except Exception as exc:
        handles = []
        w("  取句柄失败: %s" % str(exc)[:110])

    target_h = None
    for h in handles:
        try:
            data = sds.k2_find_subobject_data_from_handle(h)
            nm = None
            for getter in ("get_variable_name", "get_object", "get_display_name"):
                fn = getattr(sds, getter, None) or getattr(data, getter, None)
                if fn is None:
                    continue
                try:
                    val = fn(data) if getattr(fn, "__self__", None) is sds else fn()
                    nm = str(val)
                    break
                except Exception:
                    continue
            w("    句柄 -> %s" % (nm or "?"))
            if nm and VICTIM in nm:
                target_h = h
        except Exception as exc:
            w("    读句柄失败: %s" % str(exc)[:80])

    if target_h is None and handles:
        w("  按名字没认出 %s，只对第一个句柄盲试一次（免得误删别的组件）" % VICTIM)
    for h in ([target_h] if target_h is not None else handles[:1]):
        if h is None:
            continue
        for call, how in (
                (lambda: sds.k2_delete_subobject_from_instance(h, a), "(handle, actor)"),
                (lambda: sds.k2_delete_subobject_from_instance(handles[0], h),
                 "(root, handle)")):
            try:
                call()
                now = splines(a)
                w("  %s 调用成功 -> 现有 %s" % (how, ", ".join(now)))
                if VICTIM not in now:
                    w("  >>> 删掉了")
                    flush()
                    return
            except Exception as exc:
                w("  %s 失败: %s" % (how, str(exc)[:110]))
    w("")

    # --- 路 2: destroy_component(object) ---
    w("=" * 70)
    w("路 2: destroy_component —— 注意它要一个 'object' 参数")
    w("=" * 70)
    v = find_victim(a)
    if v is None:
        w("  已经没有了")
    else:
        for arg, how in ((v, "自己"), (a, "actor")):
            try:
                v.destroy_component(arg)
                now = splines(a)
                w("  传 %s 调用成功 -> 现有 %s" % (how, ", ".join(now)))
                if VICTIM not in now:
                    w("  >>> 删掉了")
                    flush()
                    return
            except Exception as exc:
                w("  传 %s 失败: %s" % (how, str(exc)[:110]))
    w("")

    w("=" * 70)
    w("都没删掉。现有样条组件: %s" % ", ".join(splines(a)))
    w("=" * 70)
    if sds is not None:
        for n in sorted(set(dir(sds))):
            if not n.startswith("_") and ("delete" in n.lower()
                                          or "remove" in n.lower()
                                          or "subobject" in n.lower()):
                w("   SubobjectDataSubsystem.%s" % n)
    v = find_victim(a)
    if v is not None:
        for n in sorted(set(dir(v))):
            if not n.startswith("_") and ("destroy" in n.lower()
                                          or "unregister" in n.lower()
                                          or "detach" in n.lower()):
                w("   SplineComponent.%s" % n)
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[delprobe] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常：")
    lines.append(err)
    flush()
