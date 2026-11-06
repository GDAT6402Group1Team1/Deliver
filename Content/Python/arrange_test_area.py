# -*- coding: utf-8 -*-
u"""把 testfortraffic 重排成"交通在一角、另一侧留给 NPC/任务测试"。

用法：编辑器控制台 `py arrange_test_area.py`。

两件事：
  1. **把地板放大**（`Floor` 的缩放）。原来缩放 8 = 半尺寸 4000，
     交通系统自己就占了 X[-2080, 2733] / Y[-2990, 2024]，挪到角上会掉出地板。
  2. **把整套交通系统整体平移到一角**。平移量对所有交通 actor 是同一个值，
     所以它们之间的相对几何完全不变——这很重要：`BP_Intersection` 是靠
     **自己的 Box 和路段的 Box 重叠**来找它管的子节点的（见 CLAUDE.md），
     各自挪不同的量会让路口找不到灯组，表现是"红绿灯没了"。

**幂等靠打标签**，不靠判断坐标：移过的 actor 打上 MOVED_TAG，重跑时跳过。
按坐标判"是不是已经在角上了"是不可靠的——用户可能自己又拖回来一点。

世界原点附近因此空了出来，而这张图**没有 PlayerStart**，玩家就出生在原点，
正好落在测试区里。所以任务点和人行路线都摆在原点周围（各自脚本里写死的坐标）。
"""

import unreal

# ---------------------------------------------------------------- 配置

EXPECTED_LEVEL = "testfortraffic"

FLOOR_LABEL = "Floor"
# 地板目标缩放。SM_Template_Map_Floor 原始半尺寸 500，所以缩放 20 = 半尺寸 10000，
# 中心在 (-410, 0) → X ∈ [-10410, 9590]、Y ∈ [-10000, 10000]。
# 交通挪到 +5500/+6300 之后占到 X 8233 / Y 8324，留得下。
FLOOR_TARGET_SCALE = 20.0

# 交通系统整体平移量（cm）。挪到 +X+Y 那个角，测试区留在原点一侧。
TRAFFIC_OFFSET = (5500.0, 6300.0)
# 认哪些是"交通系统"。**不含 BP_Motorbike**：那是玩家的车，要留在出生点附近。
TRAFFIC_CLASS_HINTS = ("BP_TrafficLine", "BP_Intersection", "trafficlight", "BP_car_base")
MOVED_TAG = "ClaudeGenTrafficMoved"

OUT = unreal.Paths.project_saved_dir() + "arrange_test_area.txt"
lines = []


def w(s=u""):
    lines.append(str(s))
    unreal.log(u"[arrange] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write(u"\n".join(lines))
        unreal.log(u"[arrange] 报告写到 %s" % OUT)
    except Exception as exc:
        unreal.log_error(u"[arrange] 写报告失败：%s" % exc)


def _is_traffic(actor):
    cls = str(actor.get_class().get_name())
    return any(h in cls for h in TRAFFIC_CLASS_HINTS)


def enlarge_floor(actors):
    u"""把地板放大到 FLOOR_TARGET_SCALE，返回 (成功, 说明)。"""
    floor = None
    for a in actors:
        if a.get_actor_label() == FLOOR_LABEL and isinstance(a, unreal.StaticMeshActor):
            floor = a
            break

    if floor is None:
        # 地板找不到就不要瞎猜：没有地板时把交通挪到角上，
        # 测试区会变成一片虚空，人走过去直接掉下去
        return False, u"找不到标签为 %s 的静态网格" % FLOOR_LABEL

    before = floor.get_actor_scale3d()
    origin, extent = floor.get_actor_bounds(False)
    w(u"  地板原状：缩放 (%.1f, %.1f, %.1f)，中心 (%.0f, %.0f, %.0f)，半尺寸 (%.0f, %.0f, %.0f)"
      % (before.x, before.y, before.z, origin.x, origin.y, origin.z,
         extent.x, extent.y, extent.z))

    if before.x >= FLOOR_TARGET_SCALE - 0.01 and before.y >= FLOOR_TARGET_SCALE - 0.01:
        return True, u"已经够大，不动"

    floor.modify(True)
    # **Z 的缩放保持原值**：地板是一块有厚度的板，中心在 z=-200、顶面刚好 z=0。
    # 跟着放大 Z 会把顶面抬到 z=+1800，所有东西都悬空了
    floor.set_actor_scale3d(unreal.Vector(FLOOR_TARGET_SCALE, FLOOR_TARGET_SCALE, before.z))

    after = floor.get_actor_scale3d()
    origin, extent = floor.get_actor_bounds(False)
    w(u"  地板现状：缩放 (%.1f, %.1f, %.1f)，半尺寸 (%.0f, %.0f, %.0f)，顶面 z=%.0f"
      % (after.x, after.y, after.z, extent.x, extent.y, extent.z, origin.z + extent.z))
    w(u"  覆盖范围：X [%.0f, %.0f]  Y [%.0f, %.0f]"
      % (origin.x - extent.x, origin.x + extent.x, origin.y - extent.y, origin.y + extent.y))

    if abs(after.x - FLOOR_TARGET_SCALE) > 0.01:
        return False, u"缩放写不进去（回读 %.2f）" % after.x
    return True, u"已放大"


def move_traffic(actors):
    u"""把交通系统整体平移到角上，返回移动的个数。"""
    dx, dy = TRAFFIC_OFFSET

    todo = []
    already = 0
    for a in actors:
        if not _is_traffic(a):
            continue
        if MOVED_TAG in [str(t) for t in a.tags]:
            already += 1
            continue
        todo.append(a)

    if already:
        w(u"  已经移过的 %d 个，跳过（靠 %s 标签判断，不靠坐标）。" % (already, MOVED_TAG))
    if not todo:
        return 0

    moved = 0
    for a in todo:
        loc = a.get_actor_location()
        target = unreal.Vector(loc.x + dx, loc.y + dy, loc.z)
        a.modify(True)
        a.set_actor_location(target, False, False)

        # 回读：set_actor_location 带 sweep 时可能被挡住而走不到位，
        # 这里 sweep 关着，但写入 API 仍然要验一次
        got = a.get_actor_location()
        if abs(got.x - target.x) > 1.0 or abs(got.y - target.y) > 1.0:
            w(u"  ！%s 没挪到位：期望 (%.0f, %.0f) 实际 (%.0f, %.0f)"
              % (a.get_actor_label(), target.x, target.y, got.x, got.y))
            continue

        a.set_editor_property("tags", list(a.tags) + [MOVED_TAG])
        moved += 1

    return moved


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is None:
        w(u"！取不到编辑器世界。")
        flush()
        return

    level_name = unreal.SystemLibrary.get_object_name(world)
    if EXPECTED_LEVEL.lower() not in level_name.lower():
        w(u"！当前关卡是 %s，不是 %s，什么都不做。" % (level_name, EXPECTED_LEVEL))
        flush()
        return

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    w(u"1. 放大地板")
    ok, note = enlarge_floor(actors)
    w(u"   %s：%s" % (u"完成" if ok else u"**失败**", note))
    if not ok:
        # 地板没放大就别挪交通：挪出去会掉到地板外面
        w(u"   地板没处理好，不挪交通（挪出去会掉到地板外）。")
        flush()
        return

    w(u"")
    w(u"2. 把交通系统整体挪到 +(%.0f, %.0f)" % TRAFFIC_OFFSET)
    moved = move_traffic(actors)
    w(u"   挪了 %d 个 actor。" % moved)

    # 挪完报一下交通的新范围和测试区的范围，省得"以为挪开了其实还压着"
    minx = miny = 1e9
    maxx = maxy = -1e9
    count = 0
    for a in eas.get_all_level_actors():
        if not _is_traffic(a):
            continue
        count += 1
        loc = a.get_actor_location()
        minx = min(minx, loc.x); maxx = max(maxx, loc.x)
        miny = min(miny, loc.y); maxy = max(maxy, loc.y)
    if count:
        w(u"   交通现在占 X [%.0f, %.0f]  Y [%.0f, %.0f]（%d 个）"
          % (minx, maxx, miny, maxy, count))
        w(u"   原点一侧空出来了：这张图没有 PlayerStart，玩家出生在原点，正好在测试区里。")

    # 玩家的车不属于交通系统，单独报一下位置，确认它没被一起挪走
    for a in eas.get_all_level_actors():
        if "BP_Motorbike" in str(a.get_class().get_name()):
            loc = a.get_actor_location()
            w(u"   玩家摩托车留在 (%.0f, %.0f, %.0f)（刻意不跟着挪）" % (loc.x, loc.y, loc.z))

    flush()


if __name__ == "__main__":
    run()
else:
    run()
