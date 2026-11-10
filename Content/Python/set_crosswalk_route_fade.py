# -*- coding: utf-8 -*-
u"""让所有横跨斑马线的路人都用渐隐出入场，而不是变大变小。

现状（2026-11-10）：只有 `Crosswalk_Intersection_45_02` 和 `Crosswalk_MainRoad_R003_9`
那四条重点路线配了渐隐，其余过街路线还在用默认的缩放出入场——一个在淡出、
一个在缩小，并排看很突兀。

机制是现成的，不用改 C++：
  * 路线上的 `bFadeInsteadOfScale` 决定用哪种出入场；
  * `FadeMaterials` 是**和 `Kinds` 同序**的材质数组，`GetFadeMaterialForMesh`
    按 `Kinds[i].Mesh == 这个人的网格` 取第 i 个材质。
    **所以顺序必须和那条路线自己的 Kinds 一致**，不能照抄一份固定顺序的六个材质——
    各条路线的 Kinds 顺序和条数都不一样（重点路线的 A 道只有男孩/女孩两种）。

六份渐隐材质都已存在（成人那四份是今天补的）。某个外观找不到对应材质时
**整条路线跳过并点名**：只配一半的表现是"同一条斑马线上有人淡出、有人缩小"，
比全都缩小更难看，也更难查。

判据是"这条路线绑了斑马线"（`CrosswalkActor` 非空），和
`set_crosswalk_route_yield.py` 同一套——游走路人不绑斑马线，不会被动到。

只读模式把 DRY_RUN 置 True。无界面：
    UnrealEditor-Cmd <工程>.uproject -run=pythonscript \
        -script=<本文件绝对路径> -unattended -nopause -nosplash -NullRHI
"""
import unreal

DRY_RUN = False
LEVEL = u"/Game/Level/TestForCharacter"
NPC_DIR = u"/Game/Characters/NPC"
# 网格名 → 渐隐材质名。和 place_child_crosswalk_45_02.py 的 NPCS 表一致
FADE_BY_MESH = {
    u"SK_NpcBoy": u"M_ChildBoy_Fade",
    u"SK_NpcGirl": u"M_ChildGirl_Fade",
    u"SK_NpcMan": u"M_Man_Fade",
    u"SK_NpcWoman": u"M_Woman_Fade",
    u"SK_NpcGrandpa": u"M_Grandpa_Fade",
    u"SK_NpcGrandma": u"M_Grandma_Fade",
}

_lines = []


def w(s=u""):
    _lines.append(s)
    unreal.log(u"[xfade] %s" % s)


def mesh_name(kind):
    u"""Kinds 里的 Mesh 是软引用，可能还没加载——按路径取名字，不要 .get()"""
    try:
        soft = kind.get_editor_property(u"mesh")
    except Exception:
        return None
    if soft is None:
        return None
    try:
        path = soft.get_path_name() if hasattr(soft, u"get_path_name") else None
    except Exception:
        path = None
    if not path:
        try:
            path = str(soft.to_tuple()[0])
        except Exception:
            path = str(soft)
    if not path or path in (u"None", u""):
        return None
    return path.split(u"/")[-1].split(u".")[0]


def run():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(LEVEL):
        w(u"！加载不了 %s" % LEVEL)
        return
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # 先把六份材质都加载一遍；缺哪一份现在就报，别等到逐条路线时才发现
    materials = {}
    for mesh, mat in FADE_BY_MESH.items():
        path = u"%s/%s.%s" % (NPC_DIR, mat, mat)
        obj = unreal.EditorAssetLibrary.load_asset(path)
        if obj is None:
            w(u"！加载不到渐隐材质 %s" % path)
        else:
            materials[mesh] = obj

    routes = [a for a in eas.get_all_level_actors()
              if a and isinstance(a, unreal.DeliveryPedestrianRoute)]
    crossing = []
    for r in routes:
        try:
            if r.get_editor_property(u"CrosswalkActor"):
                crossing.append(r)
        except Exception:
            pass

    w(u"=== 过街路线统一改成渐隐出入场 ===")
    w(u"DRY_RUN = %s" % DRY_RUN)
    w(u"路线共 %d 条，其中绑了斑马线的 %d 条（其余不动）" % (len(routes), len(crossing)))

    changed, already, skipped = 0, 0, []
    for r in sorted(crossing, key=lambda a: a.get_actor_label()):
        label = r.get_actor_label()
        kinds = list(r.get_editor_property(u"Kinds") or [])
        if not kinds:
            skipped.append((label, u"Kinds 是空的"))
            continue
        names = [mesh_name(k) for k in kinds]
        missing = [n for n in names if n not in materials]
        if missing:
            # 只配一半比全缩小更糟：同一条斑马线上会出现"有人淡出有人缩小"
            skipped.append((label, u"这些外观没有渐隐材质：%s" % u", ".join(
                [str(n) for n in missing])))
            continue

        want = [materials[n] for n in names]
        had_flag = bool(r.get_editor_property(u"bFadeInsteadOfScale"))
        had = list(r.get_editor_property(u"FadeMaterials") or [])
        same = had_flag and len(had) == len(want) and all(
            (h.get_path_name() if hasattr(h, u"get_path_name") else None)
            == want[i].get_path_name() for i, h in enumerate(had))
        if same:
            already += 1
            w(u"  = %-48s 本来就配好了（%d 种）" % (label, len(want)))
            continue
        if DRY_RUN:
            w(u"  - %-48s 要配 %d 种：%s" % (label, len(want), u", ".join(
                [FADE_BY_MESH[n] for n in names])))
            continue

        r.set_editor_property(u"FadeMaterials", want)
        r.set_editor_property(u"bFadeInsteadOfScale", True)
        # 回读验证：写实例覆盖在本工程栽过（"写完立刻读回是好的，存盘重载就变回去"）
        back = list(r.get_editor_property(u"FadeMaterials") or [])
        if not bool(r.get_editor_property(u"bFadeInsteadOfScale")) or len(back) != len(want):
            skipped.append((label, u"写了没生效（读回 %d 项）" % len(back)))
            continue
        changed += 1
        w(u"  - %-48s 已配 %d 种" % (label, len(want)))

    w(u"")
    if skipped:
        w(u"跳过 %d 条：" % len(skipped))
        for label, why in skipped:
            w(u"  ! %-48s %s" % (label, why))
    w(u"改了 %d 条、本来就对的 %d 条" % (changed, already))

    if DRY_RUN:
        w(u"DRY_RUN：什么都没写。")
        return
    if skipped:
        w(u"！有跳过的条目，不保存关卡——先把原因解决掉，避免半套配置留在图里。")
        return
    if changed:
        les.save_current_level()
        w(u"已保存关卡（只改了出入场方式和渐隐材质，没动位置/样条/生成参数）。")
    else:
        w(u"没有要改的。")


run()
