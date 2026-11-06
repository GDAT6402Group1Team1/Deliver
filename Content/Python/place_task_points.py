# -*- coding: utf-8 -*-
u"""在 testfortraffic 里摆一套和主地图一样的取货点 + 收件点（Task_001）。

用法：编辑器控制台 `py place_task_points.py`。幂等——重跑先删掉上次摆的再摆一遍。

**落点是写死的两个坐标，不去找路**。这张图是手摆的单路口小图
（十字路口在原点，两条路沿 ±X/±Y，整张图只有 ±2500）。
想换位置直接改下面两个常量，或者摆完在编辑器里拖——Actor 上没有任何依赖位置的配置。

地板是 `Floor`（`SM_Template_Map_Floor`），顶面 z=0，被 `arrange_test_area.py`
放大到半尺寸 10000，所以原点周围这一片都站得住。

（试过按车道样条自动算落点，对这张图是过度设计：那套逻辑要同时处理
生成图的 `Lane_*` 和手摆图的 `BP_TrafficLine*` 两种形状、还要按车道间距推侧移距离，
而这里只是要个能按 E 的地方。）

主地图（TestForCharacter）里这套是怎么配的（逐项对过它的 umap）：

  * 取货点：一个 ADeliveryPickupPoint，`LocationId = Location_Restaurant_001`，
    **PackageClass 留空**（留空时退回内置测试快递，所以不用再配快递蓝图）。
  * 收件点：地点 + 可交互 + 收件三件套，`LocationId = Location_OldManHouse_001`、
    `ExpectedTask = DA_Task_001`。这里用现成的 ADeliveryDropOffPoint，
    三个组件在 C++ 构造函数里就建好了——**Python 加的组件存不住**
    （实例上 destroy/add 当场生效，重载后按蓝图 SCS 重建），所以必须用现成 Actor。
  * 任务清单在 BP_DeliveryGameState 的 `TaskDefinitions` 上，已经包含 DA_Task_001；
    GameMode 是项目全局默认（`BP_DeliverGameMode`），**testfortraffic 没有覆盖**，
    所以这张图上任务系统本来就是通的，只缺这两个点。

地点编号刻意和主地图**用同一个**：`Tasks.csv` 里 Task_001 写的就是这两个 ID，
换个新 ID 就得同步改策划表和任务资产。两张图不会同时加载，同 ID 不冲突。

流程验收（进 PIE）：
  Delivery.Task.Validate  一次列全配置问题（ID 解析不出来、收件点没填 ExpectedTask…）
  Delivery.Pickup.Dump    这个柜台此刻该发哪一单、发不出的原因
  Delivery.DropOff.Dump   收件点收哪个任务、你和快递各自离测量点多远
  按 J 接电话（必须接听过才能取件）→ 走到取货点按住 E 领货 → 到收件点按住 E 交付
"""

import math

import unreal

# ---------------------------------------------------------------- 配置

EXPECTED_LEVEL = "testfortraffic"
TAG = "ClaudeGenTaskPoint"

TASK_ASSET = "/Game/Task/Definitions/DA_Task_001"
PICKUP_LOCATION_ID = "Location_Restaurant_001"
DROPOFF_LOCATION_ID = "Location_OldManHouse_001"

PICKUP_LABEL = "Task001_PickupPoint"
DROPOFF_LABEL = "Task001_DropOffPoint"

# 落点（世界 XY）。交通系统已经被 arrange_test_area.py 整体挪到 +X+Y 那一角，
# 所以原点这一侧是空地，两个点摆在这儿。
# 这张图**没有 PlayerStart**，玩家出生在世界原点，正好在两点中间。
# 相距 5000 cm（50m）：够测罗盘、脚边箭头和小地图的指向，骑车十几秒就到。
PICKUP_XY = (-2000.0, -1500.0)
DROPOFF_XY = (2000.0, 1500.0)

# 玩家离多近能按 E。**这是唯一的填写位置**：组件每次 BeginPlay 都会把它写进
# 可交互组件，所以"调了不生效"不可能发生。
INTERACT_RADIUS = 200.0

OUT = unreal.Paths.project_saved_dir() + "place_task_points.txt"
lines = []


def w(s=u""):
    lines.append(str(s))
    unreal.log(u"[taskpoint] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write(u"\n".join(lines))
        unreal.log(u"[taskpoint] 报告写到 %s" % OUT)
    except Exception as exc:
        unreal.log_error(u"[taskpoint] 写报告失败：%s" % exc)


def _ground_point(world, x, y):
    u"""(x, y) 这一竖线上的地面点，探不到就退回 z=0 并点名。

    **不筛 SplineMesh**（和车道脚本相反）：点要落在路面/人行道/地形上，
    只认路面的话会探不到、把点摆到地下。
    """
    try:
        hit = unreal.SystemLibrary.line_trace_single(
            world, unreal.Vector(x, y, 5000.0), unreal.Vector(x, y, -5000.0),
            unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
            False, [], unreal.DrawDebugTrace.NONE, True)
    except Exception:
        hit = None

    if hit is not None:
        d = hit.to_dict()
        if d.get("blocking_hit"):
            return unreal.Vector(x, y, float(d["impact_point"].z) + 5.0), True

    w(u"  (%.0f, %.0f) 探不到地面，Z 退回 0。摆完在编辑器里拖一下高度就行。" % (x, y))
    return unreal.Vector(x, y, 5.0), False


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is None:
        w(u"！取不到编辑器世界。")
        flush()
        return

    level_name = unreal.SystemLibrary.get_object_name(world)
    if EXPECTED_LEVEL.lower() not in level_name.lower():
        # 刻意不"顺手摆到当前图上"：静默摆进别的关卡之后，
        # 谁都不会想到去别的图里找这两个 actor
        w(u"！当前关卡是 %s，不是 %s，什么都不做。" % (level_name, EXPECTED_LEVEL))
        flush()
        return

    pickup_class = getattr(unreal, "DeliveryPickupPoint", None)
    dropoff_class = getattr(unreal, "DeliveryDropOffPoint", None)
    if pickup_class is None or dropoff_class is None:
        w(u"！取不到 unreal.DeliveryPickupPoint / DeliveryDropOffPoint，"
          u"C++ 没编译或只编了 Delivery 没编 DeliveryEditor。")
        flush()
        return

    task = unreal.load_asset(TASK_ASSET)
    if task is None:
        w(u"！加载不到任务资产 %s。收件点不填 ExpectedTask 的表现是"
          u"「走过去按 E 没反应」，所以这里直接停下。" % TASK_ASSET)
        flush()
        return
    w(u"任务资产：%s" % task.get_path_name())

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    pickup_pos, p_hit = _ground_point(world, PICKUP_XY[0], PICKUP_XY[1])
    drop_pos, d_hit = _ground_point(world, DROPOFF_XY[0], DROPOFF_XY[1])
    gap = math.sqrt((pickup_pos.x - drop_pos.x) ** 2 + (pickup_pos.y - drop_pos.y) ** 2)

    # 幂等：按 tag 删掉上次摆的。按 tag 而不是按名字——改过 label 的话
    # 按名字匹配会漏掉，然后每跑一次多一对点
    removed = 0
    for a in eas.get_all_level_actors():
        try:
            if TAG in [str(t) for t in a.tags]:
                eas.destroy_actor(a)
                removed += 1
        except Exception:
            pass
    if removed:
        w(u"删掉旧的任务点 %d 个。" % removed)

    rot = unreal.Rotator(0.0, 0.0, 0.0)

    pickup = eas.spawn_actor_from_class(pickup_class, pickup_pos, rot)
    if pickup is None:
        w(u"！取货点生成失败。")
        flush()
        return
    pickup.set_actor_label(PICKUP_LABEL)
    pickup.set_editor_property("tags", [TAG])
    pickup.set_editor_property("location_id", PICKUP_LOCATION_ID)
    pickup.set_editor_property("interact_radius", INTERACT_RADIUS)
    # PackageClass 刻意留空：留空退回内置测试快递，和主地图一致。
    # 填错一个类比留空更难查——会变成"领到一件不是快递的东西"

    drop = eas.spawn_actor_from_class(dropoff_class, drop_pos, rot)
    if drop is None:
        w(u"！收件点生成失败。")
        flush()
        return
    drop.set_actor_label(DROPOFF_LABEL)
    drop.set_editor_property("tags", [TAG])
    drop.set_editor_property("location_id", DROPOFF_LOCATION_ID)
    drop.set_editor_property("expected_task", task)
    drop.set_editor_property("interact_radius", INTERACT_RADIUS)

    # 回读验证。写入 API 可能静默失败，而"脚本跑过了"证明不了值留住了
    ok = True
    for actor, field, expected in ((pickup, "location_id", PICKUP_LOCATION_ID),
                                   (drop, "location_id", DROPOFF_LOCATION_ID)):
        got = str(actor.get_editor_property(field))
        if got != expected:
            w(u"！%s 的 %s 回读是 %s，期望 %s。" % (actor.get_actor_label(), field, got, expected))
            ok = False
    got_task = drop.get_editor_property("expected_task")
    if got_task is None or str(got_task.get_path_name()) != task.get_path_name():
        w(u"！收件点的 ExpectedTask 回读不对。")
        ok = False

    w(u"")
    w(u"取货点 %s：%s @ (%.0f, %.0f, %.0f)%s"
      % (PICKUP_LABEL, PICKUP_LOCATION_ID, pickup_pos.x, pickup_pos.y, pickup_pos.z,
         u"" if p_hit else u"（高度是兜底值）"))
    w(u"收件点 %s：%s + %s @ (%.0f, %.0f, %.0f)%s"
      % (DROPOFF_LABEL, DROPOFF_LOCATION_ID, task.get_name(),
         drop_pos.x, drop_pos.y, drop_pos.z, u"" if d_hit else u"（高度是兜底值）"))
    w(u"两点相距 %.0f cm，交互半径各 %.0f cm。" % (gap, INTERACT_RADIUS))
    w(u"回读校验：%s" % (u"通过" if ok else u"**有项目没写进去**"))
    w(u"")
    w(u"进 PIE 后：Delivery.Task.Validate 一次列全配置问题；")
    w(u"按 J 接电话（必须接听过才能取件）→ 走到取货点按住 E 领货 → 到收件点按住 E 交付。")
    w(u"大纲里搜 %s / %s 可以定位，直接拖着挪位置。" % (PICKUP_LABEL, DROPOFF_LABEL))
    flush()


if __name__ == "__main__":
    run()
else:
    run()
