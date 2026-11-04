# -*- coding: utf-8 -*-
u"""路人一条龙：导入 Design/Art/NPC/*.fbx → 共用一个 Skeleton → 在 testfortraffic 摆一条人行路线。

用法：编辑器控制台 `py setup_npc.py`。
前提：C++ 已经编译过（要有 unreal.DeliveryPedestrianRoute 这个类）。

两步各自幂等，可以单独重跑：

    import_npcs()        导入 Design/Art/NPC/ 下的 FBX，第一个建 Skeleton、其余复用
    place_route()        在 testfortraffic 的人行道上摆一条 ADeliveryPedestrianRoute

--------------------------------------------------------------------------
这几份 FBX 的事实（离线解析过，改导入流程前先读）
--------------------------------------------------------------------------
* 有蒙皮（Skin 1 / Cluster 29），骨骼是 **Mixamo 标准命名**
  （Hips / Spine→Spine2 / Neck / Head / LeftUpLeg / LeftLeg / LeftArm …）。
  六个角色骨骼名完全一致，所以**共用一个 Skeleton**：以后一份走路动画六个人通用。
* **没有走路动画**：AnimStack 有 1 个，但 AnimCurveNode 和 AnimLayer 都是 0，是个空壳。
* 导入会**顺带生成物理资产**（`create_physics_asset=True`）：被打飞/撞飞时路人要切成
  真布娃娃吃冲量，没有物理资产就只能原地退场。
  所以路人默认走 C++ 的程序化步态（见 ADeliveryPedestrian::UpdateProceduralGait）；
  以后往路线的 WalkAnim 里填一份真动画，C++ 会自动切到播放动画那条路。
* 绑定姿势就是站姿，所以 **use_t0_as_ref_pose 保持关闭**。
  摩托车骑手那次要开，是因为那份 FBX 的坐姿写在骨骼节点的当前变换里，这里不是。
* 外部贴图文件名一个都扫不到、内嵌也只有一处，**大概率导进来是灰白模型**。
  要彩色就得拿到贴图，或者在 Blender 里用 Path Mode: Copy + Embed Textures 重导一次。

--------------------------------------------------------------------------
缩放是量出来的，不是填的
--------------------------------------------------------------------------
先按 1:1 导入，量出"中年男"的包围盒高度，再按 TARGET_ADULT_HEIGHT 反推一个
**六个角色共用的**缩放系数，然后干净重导一次。共用一个系数是刻意的：
各自拉到同一身高会把小孩拉成大人，模型之间本来的高矮关系才是对的。

重导必须**先删再导**，不能靠 replace_existing：带着已有资产做重导会被路由到
Interchange，FbxImportUI 的选项（含缩放）会被整份忽略（摩托车那边记过这一条）。
"""

import math
import traceback

import unreal

# ---------------------------------------------------------------- 配置

# 源美术文件统一放在 Design/Art/ 下（相对工程目录），不散在工程根目录
FBX_DIR = u"Design/Art/NPC"
DEST = "/Game/Characters/NPC"

# 文件名 → 资产名。资产名用拼音/英文：中文资产名在引用、重定向和命令行里都很难伺候。
NPC_FILES = [
    (u"小男孩.fbx", "SK_NpcBoy", u"小男孩"),
    (u"小女孩.fbx", "SK_NpcGirl", u"小女孩"),
    (u"中年男.fbx", "SK_NpcMan", u"中年男"),
    (u"中年女.fbx", "SK_NpcWoman", u"中年女"),
    (u"老爷爷.fbx", "SK_NpcGrandpa", u"老爷爷"),
    (u"老奶奶.fbx", "SK_NpcGrandma", u"老奶奶"),
]

# 共用的 Skeleton 由第一个成功导入的角色创建，其余全部复用它。
# 必须是成年人：Skeleton 存的是参考骨架的比例，用小孩建的话以后导入的
# Mixamo 动画会按小孩的骨长重定向，成年人身上会变形。
SKELETON_SOURCE = "SK_NpcMan"
SKELETON_ASSET = DEST + "/SK_Npc_Skeleton"

# 量出来的目标身高（cm）。**主角实测约 140 cm**：BP_DeliveryMan 的胶囊半高是 70
# （不是 CLAUDE.md 早先记的 96），renwu 网格的包围盒高 139.9、正好对得上。
# 按要求中年路人和主角一样高，所以基准取 140，中年系数 1.0 就等于主角身高。
# **别按胶囊 96 反推 192**：那会让路人整体比主角高出一头，第一版就是这么错的。
TARGET_ADULT_HEIGHT = 140.0
# 身高量在"中年男"上，其余角色跟着同一个系数缩放，保留相对高矮。
HEIGHT_REFERENCE = "SK_NpcMan"
# 实测身高落在这个区间就认为缩放已经对了，不再重导（避免每次跑都重导一遍）。
HEIGHT_TOLERANCE = 8.0

# 摆路线：只碰这张图。
EXPECTED_LEVEL = "testfortraffic"
ROUTE_TAG = "ClaudeGenPedestrianRoute"
ROUTE_LABEL = "PedestrianRoute_Test"

# **一张小路网**，不是一条线。路人走到端点会换一条相连的路线继续走
# （ADeliveryPedestrianRoute 的接驳），所以这里给的是若干首尾相接的线段。
#
# 端点**必须真的重合**（或者差距小于路线的 ConnectDistance，默认 400），
# 连通判据只看端点到端点——中段靠得近的两条路不算连通，否则路人会在路中央凭空拐弯。
#
# 画的是一个"目"字形：一条南北主街 + 两条东西横街 + 一条回头路，
# 这样路口有分叉（走到路口会随机挑一条），而不是一条来回走的死路。
# 坐标都在原点一侧的测试区里，交通已经被 arrange_test_area.py 挪到 +X+Y 角。
ROUTE_Z = 5.0
PEDESTRIAN_ROUTES = [
    # (名字, [(x, y), ...])
    ("Main_NS",   [(-600.0, -3000.0), (-600.0, 3000.0)]),
    ("Cross_S",   [(-600.0, -3000.0), (-3200.0, -3000.0)]),
    ("Cross_N",   [(-600.0, 3000.0), (-3200.0, 3000.0)]),
    ("Back_NS",   [(-3200.0, -3000.0), (-3200.0, 3000.0)]),
]

# (资产名, 权重, 身高系数)
# 只勾两个小孩：先按要求测试小男孩小女孩，其余四个导入了但权重留 0，随时能开。
#
# **身高系数必须在这里写**：六份源模型按老的 FbxFactory 路线、缩放 1.0 导进来的
# 包围盒高度实测全是 99.8 cm（归一化过的），同一个导入缩放会把所有人拉到
# TARGET_ADULT_HEIGHT，所以小男孩和中年男一样高。
# 被 Interchange 接走时同样的调用量到的是 192 cm（那条路忽略 FbxImportUI 的缩放、
# 按自己的单位换算），所以报告里"导入路"那一列要看一眼，两个数不是同一回事。
# 相对高矮不在模型里，只能在这儿表达。系数是相对 140 cm（主角身高）的：
#   中年 1.0 = 140 cm（和主角一样高）、老人 4/5 = 0.8 = 112 cm、小孩 3/5 = 0.6 = 84 cm
# 六种全部开权重，否则改了身高在图里也看不出来（只有小孩会出现）。
INITIAL_KINDS = [("SK_NpcBoy", 1.0, 0.60), ("SK_NpcGirl", 1.0, 0.60),
                 ("SK_NpcMan", 1.0, 1.00), ("SK_NpcWoman", 1.0, 1.00),
                 ("SK_NpcGrandpa", 1.0, 0.80), ("SK_NpcGrandma", 1.0, 0.80)]

WS = unreal.SplineCoordinateSpace.WORLD

OUT = unreal.Paths.project_saved_dir() + "setup_npc.txt"
lines = []


def w(s=u""):
    lines.append(str(s))
    unreal.log(u"[npc] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write(u"\n".join(lines))
        unreal.log(u"[npc] 报告写到 %s" % OUT)
    except Exception as exc:
        unreal.log_error(u"[npc] 写报告失败：%s" % exc)


def _tools():
    return unreal.AssetToolsHelpers.get_asset_tools()


def _fbx_path(name):
    return unreal.Paths.project_dir() + FBX_DIR + "/" + name


# ---------------------------------------------------------------- 1. 导入

def _make_task(fbx, asset_name, scale, skeleton):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", fbx)
    task.set_editor_property("destination_path", DEST)
    task.set_editor_property("destination_name", asset_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)

    ui = unreal.FbxImportUI()
    ui.set_editor_property("import_mesh", True)
    ui.set_editor_property("import_as_skeletal", True)
    ui.set_editor_property("import_materials", True)
    ui.set_editor_property("import_textures", True)
    # 这些 FBX 里的 AnimStack 是空壳（AnimCurveNode 0），导它只会多出一个空动画
    ui.set_editor_property("import_animations", False)
    # **必须生成物理资产**：路人平时是运动学推进的，但被打飞/撞飞那一刻要切成
    # 真布娃娃吃冲量，没有物理资产就模拟不了。
    # 第一版这里是 False（"路人不进物理，物理资产纯属多余"），结果加受击的时候
    # 六个角色一个能躺的都没有，C++ 侧只能退回"原地停住再缩掉"。
    ui.set_editor_property("create_physics_asset", True)
    ui.set_editor_property("automated_import_should_detect_type", False)
    ui.set_editor_property("mesh_type_to_import",
                           unreal.FBXImportType.FBXIT_SKELETAL_MESH)

    # 复用同一个 Skeleton，六个角色的骨骼名完全一致（已逐个解析确认）。
    # 不复用的话会得到六套 Skeleton，以后一份走路动画要重定向六次。
    if skeleton is not None:
        ui.set_editor_property("skeleton", skeleton)

    data = ui.get_editor_property("skeletal_mesh_import_data")
    data.set_editor_property("import_morph_targets", False)
    # **保持关闭**。这几份 FBX 的绑定姿势就是站姿，要的正是它。
    # 摩托车骑手那次要开，是因为那份的坐姿写在骨骼节点的当前变换里，这里不是；
    # 在这里开着反而会用 t0 的节点变换覆盖绑定姿势，姿态可能跑偏。
    data.set_editor_property("use_t0_as_ref_pose", False)
    data.set_editor_property("import_uniform_scale", scale)
    data.set_editor_property("convert_scene", True)
    task.set_editor_property("options", ui)

    # 关键一行。AssetTools::ImportAssetTasks 里：
    #     bUseInterchangeFramework = IsInterchangeImportEnabled() && (SpecifiedFactory == nullptr)
    # UE 5.8 默认用 Interchange 接管 FBX，那条路不读 FbxImportUI，上面的选项会被整份忽略。
    try:
        task.set_editor_property("factory", unreal.FbxFactory())
    except Exception as exc:
        w(u"！指定 FbxFactory 失败（%s）：导入会走 Interchange，上面的选项可能不生效。" % exc)

    return task


def _run_task(task):
    _tools().import_asset_tasks([task])
    for path in (task.get_editor_property("imported_object_paths") or []):
        asset = unreal.load_asset(path)
        if isinstance(asset, unreal.SkeletalMesh):
            return asset
    # 落点上直接取一次：有些版本不填 imported_object_paths
    asset = unreal.load_asset("%s/%s.%s" % (DEST, task.get_editor_property("destination_name"),
                                            task.get_editor_property("destination_name")))
    return asset if isinstance(asset, unreal.SkeletalMesh) else None


def _mesh_height(mesh):
    u"""骨骼网格的包围盒高度（cm）。"""
    try:
        box = mesh.get_bounds().box_extent
        return float(box.z) * 2.0
    except Exception:
        return 0.0


def _import_data_kind(asset):
    u"""报告走了哪条导入路。别假设，查一下——

    FbxSkeletalMeshImportData = 老的 FbxFactory（选项有效）
    InterchangeAssetImportData = 被 Interchange 接走（选项被整份忽略）
    这一行让"选项读回 True、结果却全错"从一个查半天的谜变成一条日志。
    """
    try:
        return asset.get_editor_property("asset_import_data").get_class().get_name()
    except Exception:
        return "?"


def _delete(asset_name):
    path = "%s/%s" % (DEST, asset_name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.delete_asset(path)
    return False


def import_npcs(scale=None):
    u"""导入全部 NPC。scale 留空时先 1:1 导一遍量身高，再按量出来的系数重导。"""
    missing = [n for n, _a, _l in NPC_FILES if not unreal.Paths.file_exists(_fbx_path(n))]
    if len(missing) == len(NPC_FILES):
        w(u"！%s 下一个 FBX 都没找到，先把模型放回 %s/。"
          % (FBX_DIR, FBX_DIR))
        return {}
    if missing:
        w(u"！这几份找不到，跳过：%s" % u"、".join(missing))

    unreal.EditorAssetLibrary.make_directory(DEST)

    first_pass = scale is None
    use_scale = 1.0 if first_pass else scale

    # 顺序刻意把 SKELETON_SOURCE 排到最前：它建 Skeleton，其余复用。
    order = sorted(NPC_FILES, key=lambda t: 0 if t[1] == SKELETON_SOURCE else 1)

    skeleton = None
    if unreal.EditorAssetLibrary.does_asset_exist(SKELETON_ASSET):
        skeleton = unreal.load_asset(SKELETON_ASSET)

    imported = {}
    for fbx_name, asset_name, label in order:
        fbx = _fbx_path(fbx_name)
        if not unreal.Paths.file_exists(fbx):
            continue

        # 先删再导。带着已有资产做 replace_existing 会被路由到 Interchange，
        # FbxImportUI 的选项（包括缩放）会被整份忽略——那会让"缩放改了没生效"
        # 看起来像是脚本没跑。
        _delete(asset_name)

        try:
            mesh = _run_task(_make_task(fbx, asset_name, use_scale, skeleton))
        except Exception as exc:
            w(u"！%s 导入抛异常：%s" % (label, exc))
            w(traceback.format_exc())
            continue

        if mesh is None:
            w(u"！%s 导入后取不到 SkeletalMesh。" % label)
            continue

        imported[asset_name] = mesh
        height = _mesh_height(mesh)
        w(u"  %s → %s：高 %.1f cm（缩放 %.3f，导入路 %s）"
          % (label, asset_name, height, use_scale, _import_data_kind(mesh)))

        if skeleton is None:
            try:
                skeleton = mesh.get_editor_property("skeleton")
            except Exception:
                skeleton = None
            if skeleton is not None:
                w(u"  共用 Skeleton 由 %s 创建：%s" % (asset_name, skeleton.get_path_name()))

    if not imported:
        return {}

    # 身高校正。只在第一趟（没指定缩放）时做，而且只有偏差超出容差才重导，
    # 否则每次跑这个脚本都会白白重导六个资产。
    if first_pass:
        reference = imported.get(HEIGHT_REFERENCE)
        if reference is None:
            w(u"！量身高的基准 %s 没导进来，跳过缩放校正。" % HEIGHT_REFERENCE)
        else:
            height = _mesh_height(reference)
            if height <= 0.01:
                w(u"！%s 的包围盒高度量成 0，跳过缩放校正。" % HEIGHT_REFERENCE)
            elif abs(height - TARGET_ADULT_HEIGHT) <= HEIGHT_TOLERANCE:
                w(u"  身高 %.1f cm 已在目标 %.0f±%.0f 内，不重导。"
                  % (height, TARGET_ADULT_HEIGHT, HEIGHT_TOLERANCE))
            else:
                fixed = TARGET_ADULT_HEIGHT / height
                w(u"  %s 高 %.1f cm，按目标 %.0f cm 反推共用缩放 %.4f，重导一遍。"
                  % (HEIGHT_REFERENCE, height, TARGET_ADULT_HEIGHT, fixed))
                return import_npcs(scale=fixed)

    # 导入产物必须显式存盘。只存蓝图、不存网格的话，用户一关编辑器资产就没了，
    # 盘上只剩引用断掉的壳（摩托车那次栽过）。
    unreal.EditorAssetLibrary.save_directory(DEST, only_if_is_dirty=False, recursive=True)
    w(u"  已存盘 %s" % DEST)
    return imported


# ---------------------------------------------------------------- 2. 摆路线

def _editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def _ground_z(world, x, y, z_hint):
    u"""这一竖线上离 z_hint 最近的实体表面高度，取不到就 None。

    和 spawn_test_cars.road_surface_z 同一套逐层下扫的做法，但这里**不筛 SplineMesh**：
    人行道可能是地形也可能是建筑边沿，只认路面的话人会被摆到地下。
    """
    top = z_hint + 2000.0
    bottom = z_hint - 2000.0
    try:
        hit = unreal.SystemLibrary.line_trace_single(
            world, unreal.Vector(x, y, top), unreal.Vector(x, y, bottom),
            unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
            False, [], unreal.DrawDebugTrace.NONE, True)
    except Exception:
        return None
    if hit is None:
        return None
    d = hit.to_dict()
    if not d.get("blocking_hit"):
        return None
    return float(d["impact_point"].z)


def place_route():
    u"""摆一张首尾相接的人行路网（幂等）。

    端点写死在 PEDESTRIAN_ROUTES 里。摆完在编辑器里拖端点就能改——
    但**拖开之后两条路就不连通了**（判据是端点距离 < ConnectDistance，默认 400），
    表现是路人走到那儿就缩掉消失，而不是拐过去。要改路网记得两头一起拖。
    """
    world = _editor_world()
    if world is None:
        w(u"！取不到编辑器世界。")
        return None

    level_name = unreal.SystemLibrary.get_object_name(world)
    if EXPECTED_LEVEL.lower() not in level_name.lower():
        # 刻意不"顺手摆到当前图上"：静默摆进别的关卡之后，
        # 谁都不会想到去别的图里找那些 actor
        w(u"！当前关卡是 %s，不是 %s，什么都不做。" % (level_name, EXPECTED_LEVEL))
        return None

    route_class = getattr(unreal, "DeliveryPedestrianRoute", None)
    if route_class is None:
        w(u"！取不到 unreal.DeliveryPedestrianRoute：C++ 还没编译，"
          u"或者只编了 Delivery 没编 DeliveryEditor。")
        return None

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # 外观只解析一次，四条路线共用同一份配置
    kinds = []
    for asset_name, weight, height in INITIAL_KINDS:
        path = "%s/%s.%s" % (DEST, asset_name, asset_name)
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.SkeletalMesh):
            w(u"  外观 %s 加载不到，跳过（先跑 import_npcs()）。" % asset_name)
            continue
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property("mesh", mesh)
        kind.set_editor_property("weight", weight)
        kind.set_editor_property("height_scale", height)
        kinds.append(kind)
    if not kinds:
        w(u"！一个外观都没配上，路线会是空的。")

    # 幂等：按 tag 删掉上次摆的。按 tag 而不是按名字——
    # 改过 label 的话按名字匹配会漏掉，然后越摆越多
    removed = 0
    for a in eas.get_all_level_actors():
        try:
            if ROUTE_TAG in [str(t) for t in a.tags]:
                eas.destroy_actor(a)
                removed += 1
        except Exception:
            pass
    if removed:
        w(u"  删掉旧路线 %d 条。" % removed)

    made = []
    for name, points in PEDESTRIAN_ROUTES:
        ends = []
        for x, y in points:
            z = _ground_z(world, x, y, ROUTE_Z)
            if z is None:
                # commandlet 里没有完整物理场景，射线常常恒不命中，所以这不算异常；
                # 路人每帧还会自己贴地，这里只要给样条一个合理的起点
                z = ROUTE_Z
            ends.append(unreal.Vector(x, y, z + 2.0))

        route = eas.spawn_actor_from_class(route_class, ends[0], unreal.Rotator(0.0, 0.0, 0.0))
        if route is None:
            w(u"！%s 生成失败。" % name)
            continue
        label = "%s_%s" % (ROUTE_LABEL, name)
        route.set_actor_label(label)
        route.set_editor_property("tags", [ROUTE_TAG])

        spline = route.get_component_by_class(unreal.SplineComponent)
        if spline is None:
            w(u"！%s 上没有样条组件，C++ 侧的 Route 组件没建起来？" % label)
            continue

        # **写点之前先 modify(True)**。clear/add/update 这类普通函数调用只改内存、
        # 不把对象标记为已修改，保存时整个对象不被序列化——现象是写完立刻读回是好的，
        # 存盘重载就变回默认值（本项目在这条上栽过，存活率从 1/8 变成 8/8）
        spline.modify(True)
        route.modify(True)
        spline.clear_spline_points(False)
        for p in ends:
            spline.add_spline_point(p, WS, False)
        for i in range(len(ends)):
            spline.set_spline_point_type(i, unreal.SplinePointType.LINEAR, False)
        spline.set_closed_loop(False, False)
        spline.update_spline()

        route.set_editor_property("kinds", kinds)
        # 四条路线分担人数，每条给少一点——接驳之后人会在路网里流动，
        # 每条都按单线那套给 6 个的话一眼看过去街上太挤
        route.set_editor_property("max_alive", 3)

        made.append((label, spline.get_spline_length(), ends))

    # 回读验证：连通性靠端点重合，所以把每条的端点打出来，便于人工核对
    w(u"")
    for label, length, ends in made:
        w(u"  %s：%.0f cm  (%.0f, %.0f) → (%.0f, %.0f)"
          % (label, length, ends[0].x, ends[0].y, ends[-1].x, ends[-1].y))

    # 端点重合核对：算一遍两两端点距离，报告哪些端点会被判为连通。
    # 不报的话"路人走到路口就消失"要靠猜，而根因就是这里差了几厘米
    w(u"")
    w(u"  端点连通核对（阈值 = 路线的 ConnectDistance，默认 400）：")
    ends_all = []
    for label, _length, ends in made:
        ends_all.append((label + "[起]", ends[0]))
        ends_all.append((label + "[终]", ends[-1]))
    linked = 0
    for i in range(len(ends_all)):
        for j in range(i + 1, len(ends_all)):
            na, pa = ends_all[i]
            nb, pb = ends_all[j]
            if na.split("[")[0] == nb.split("[")[0]:
                continue
            d = math.sqrt((pa.x - pb.x) ** 2 + (pa.y - pb.y) ** 2)
            if d <= 400.0:
                linked += 1
                w(u"    %s ↔ %s  相距 %.0f cm" % (na, nb, d))
    w(u"  共 %d 处连通。" % linked)
    if linked == 0:
        w(u"  ！一处都不连通，路人走到端点会直接缩掉消失，和没做接驳一样。")

    enabled = [u"%s×%.2f" % (n, h) for n, wt, h in INITIAL_KINDS if wt > 0.0]
    w(u"  %d 条路线，每条上限 3 人，启用外观：%s" % (len(made), u"、".join(enabled)))
    w(u"  在大纲里搜 %s 可以定位。" % ROUTE_LABEL)
    return made


# ---------------------------------------------------------------- 入口

def run():
    w(u"=== 路人一条龙 ===")
    w(u"")
    w(u"1. 导入 NPC")
    meshes = import_npcs()
    w(u"   导入 %d 个。" % len(meshes))
    w(u"")
    w(u"2. 在 %s 摆人行路线" % EXPECTED_LEVEL)
    place_route()
    w(u"")
    w(u"验收：进 PIE，控制台 Delivery.Pedestrian.Dump 看在场路人和配置问题；")
    w(u"      Delivery.Pedestrian.Spawn 立刻补一个；Delivery.Pedestrian.Clear 清场。")
    w(u"注意：这几份 FBX 里扫不到贴图，路人很可能是灰白的——那不是导入出错。")
    flush()


if __name__ == "__main__":
    run()
else:
    run()
