# -*- coding: utf-8 -*-
"""摩托车一条龙：导入 FBX → 建 BP_Motorbike → 放进当前关卡 → 配好 F 键。

用法：编辑器控制台 `py setup_motorbike.py`，或菜单 Delivery → Setup Motorbike。
前提：C++ 已经编译过（要有 unreal.DeliveryMotorbike 这个类）。

四步各自幂等，可以单独重跑（控制台里 `import setup_motorbike as m; m.place_in_level()`）：

    import_assets()    导入 摩托车.fbx
    setup_input()      建 IA_Interact + 在 IMC_Default 上把 F 映射过去
    build_blueprint()  建/更新 /Game/Vehicle/Motorbike/BP_Motorbike
    place_in_level()   在当前关卡出生点前方放一辆

--------------------------------------------------------------------------
为什么要导入两次（这份 FBX 的坑，改之前先读）
--------------------------------------------------------------------------
摩托车.fbx 里有两套东西：

  * 骑手：Character_Body / Eye_Left / Eye_Right，蒙皮在 mixamorig 骨架上；
  * 车体：9 个没有蒙皮的网格，挂在场景根节点下，**不在骨架层级里**。

离线解析这份 FBX 得到的关键事实是：**坐姿写在骨骼的当前变换里，不在网格顶点里**。
绑定姿势（Cluster 的 TransformLink）是站姿——膝盖在髋正下方；而骨骼节点的当前
变换是坐姿——大腿前伸下压 131 度、小腿回折 55 度、两只手落在把手宽度上。

推论：

  * 按**骨骼网格**导入 → 参考骨架取自节点当前变换 = 坐姿，导出来就是坐着的人。✔
  * 按**静态网格**导入 → 只有原始顶点 = 站姿，人会站在车里。✘

而车体那 9 个网格没有蒙皮，骨骼网格导入器会直接跳过它们。所以只能一份 FBX 导两次：
骨骼网格拿骑手，静态网格拿车体，各取所需。

静态网格这次用 combine_meshes=False + transform_vertex_to_absolute=True：
前者让 9 个部件各自成为一个资产（不然会和站姿骑手焊成一块，永远分不开），
后者让顶点留在场景绝对坐标里——于是把 9 个组件都摆在相对变换零点上就能原样拼回整车，
不需要手工还原每个部件的相对位置。
"""

import traceback

import unreal

# ---------------------------------------------------------------- 配置

FBX_NAME = u"摩托车.fbx"
DEST = "/Game/Vehicle/Motorbike"
PARTS_DEST = DEST + "/Parts"
BP_NAME = "BP_Motorbike"
BP_PATH = DEST + "/" + BP_NAME
RIDER_ASSET_NAME = "SK_MotorbikeRider"

# 导入缩放。FBX 里骑手站立高度是 100 个单位，游戏角色胶囊是 96 半高（约 192cm），
# 所以 1.9 让骑手和现有角色一样高。车体和骑手必须用同一个值，不然比例会错。
# 车体在 FBX 单位下是 135(长) x 88(高) x 72(宽)，乘 1.9 ≈ 256 x 168 x 136 cm。
# 觉得车太大就整体调小这个数，然后重跑 import_assets(force=True) + build_blueprint()。
IMPORT_SCALE = 1.9

# 骑手那三个网格按静态网格导进来是站姿的废品，识别出来直接删掉。
RIDER_MESH_NAMES = ("Character_Body", "Eye_Left", "Eye_Right")

INTERACT_IA = "/Game/Input/Actions/IA_Interact"
TEMPLATE_IA = "/Game/Input/Actions/IA_Jump"   # 复制它来建 IA_Interact
IMC_DEFAULT = "/Game/Input/IMC_Default"
INTERACT_KEY = "F"

TAG = "ClaudeGenMotorbike"
EXPECTED_LEVEL = "testfortraffic"
SPAWN_AHEAD = 450.0      # 放在出生点前方多远

OUT = unreal.Paths.project_saved_dir() + "setup_motorbike.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[motorbike] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        unreal.log("[motorbike] 报告写到 %s" % OUT)
    except Exception as exc:
        unreal.log_error("[motorbike] 写报告失败：%s" % exc)


def _tools():
    return unreal.AssetToolsHelpers.get_asset_tools()


def _fbx_path():
    return unreal.Paths.project_dir() + FBX_NAME


# ---------------------------------------------------------------- 1. 导入

def _make_task(dest, as_skeletal, combine, name=None):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", _fbx_path())
    task.set_editor_property("destination_path", dest)
    if name:
        task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)

    ui = unreal.FbxImportUI()
    ui.set_editor_property("import_mesh", True)
    ui.set_editor_property("import_as_skeletal", as_skeletal)
    ui.set_editor_property("import_materials", True)
    ui.set_editor_property("import_textures", True)
    ui.set_editor_property("import_animations", False)
    ui.set_editor_property("create_physics_asset", False)
    ui.set_editor_property("automated_import_should_detect_type", False)
    ui.set_editor_property(
        "mesh_type_to_import",
        unreal.FBXImportType.FBXIT_SKELETAL_MESH if as_skeletal else unreal.FBXImportType.FBXIT_STATIC_MESH)

    if as_skeletal:
        data = ui.get_editor_property("skeletal_mesh_import_data")
        data.set_editor_property("import_morph_targets", False)
        # 参考姿势必须取自节点当前变换（=坐姿）。打开 t0 会改用动画第 0 帧，
        # 这份 FBX 没有动画，打开等于把坐姿丢掉。
        data.set_editor_property("use_t0_as_ref_pose", False)
    else:
        data = ui.get_editor_property("static_mesh_import_data")
        data.set_editor_property("combine_meshes", combine)
        data.set_editor_property("transform_vertex_to_absolute", True)
        data.set_editor_property("auto_generate_collision", False)

    data.set_editor_property("import_uniform_scale", IMPORT_SCALE)
    data.set_editor_property("convert_scene", True)
    task.set_editor_property("options", ui)

    # 关键一行。AssetTools::ImportAssetTasks 里：
    #     bUseInterchangeFramework = IsInterchangeImportEnabled() && (SpecifiedFactory == nullptr)
    # UE 5.8 默认用 Interchange 接管 FBX，那条路不读 FbxImportUI，上面这些选项会被整份忽略。
    # 显式指定 factory 才会回到老的 FBX 导入器。
    try:
        task.set_editor_property("factory", unreal.FbxFactory())
    except Exception as exc:
        w(u"！指定 FbxFactory 失败（%s）：导入会走 Interchange，上面的选项可能不生效。" % exc)

    return task


def import_assets(force=False):
    """返回 (车体静态网格列表, 骑手骨骼网格)。"""
    fbx = _fbx_path()
    if not unreal.Paths.file_exists(fbx):
        w(u"！找不到 %s，先把模型文件放回工程根目录。" % fbx)
        return [], None

    already = unreal.EditorAssetLibrary.does_directory_exist(PARTS_DEST) and \
        len(unreal.EditorAssetLibrary.list_assets(PARTS_DEST, recursive=False)) > 0
    if already and not force:
        w(u"车体资产已存在，跳过导入（要重导就 import_assets(force=True)）。")
        return _collect_body_meshes(), _load_rider()

    w(u"导入 %s（缩放 %.2f）" % (fbx, IMPORT_SCALE))

    # 骑手：骨骼网格。车体那 9 个没蒙皮的网格会被导入器跳过，这是预期行为。
    sk_task = _make_task(DEST, True, False, RIDER_ASSET_NAME)
    _tools().import_asset_tasks([sk_task])
    sk_paths = [str(p) for p in sk_task.get_editor_property("imported_object_paths")]
    w(u"  骨骼网格导入产物 %d 个：%s" % (len(sk_paths), ", ".join(sk_paths[:6])))

    # 车体：静态网格，每个部件一个资产，顶点留在场景绝对坐标里。
    sm_task = _make_task(PARTS_DEST, False, False)
    _tools().import_asset_tasks([sm_task])
    sm_paths = [str(p) for p in sm_task.get_editor_property("imported_object_paths")]
    w(u"  静态网格导入产物 %d 个" % len(sm_paths))

    _drop_standing_rider_copies()
    body = _collect_body_meshes()
    rider = _load_rider()
    w(u"  车体部件 %d 个，骑手 %s" % (len(body), u"有" if rider else u"没有（！）"))
    return body, rider


def _drop_standing_rider_copies():
    """静态网格那一遍也会把骑手导成站姿，用不上，删掉免得以后拿错。"""
    for path in unreal.EditorAssetLibrary.list_assets(PARTS_DEST, recursive=False):
        name = path.split("/")[-1].split(".")[0]
        if any(name.startswith(r) for r in RIDER_MESH_NAMES):
            try:
                unreal.EditorAssetLibrary.delete_asset(path)
                w(u"  删掉站姿骑手副本 %s" % name)
            except Exception as exc:
                w(u"  删 %s 失败（无所谓，不会被用到）：%s" % (name, exc))


def _collect_body_meshes():
    out = []
    if not unreal.EditorAssetLibrary.does_directory_exist(PARTS_DEST):
        return out
    for path in sorted(unreal.EditorAssetLibrary.list_assets(PARTS_DEST, recursive=False)):
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if isinstance(asset, unreal.StaticMesh):
            out.append(asset)
    return out


def _load_rider():
    asset = unreal.EditorAssetLibrary.load_asset(DEST + "/" + RIDER_ASSET_NAME)
    return asset if isinstance(asset, unreal.SkeletalMesh) else None


# ---------------------------------------------------------------- 2. 输入

def setup_input():
    ia = unreal.EditorAssetLibrary.load_asset(INTERACT_IA)
    if ia is None:
        # 不用 create_asset：InputAction 没有暴露给 Python 的工厂。
        # 复制一个现成的 IA 再改属性，是这里唯一稳的做法。
        if not unreal.EditorAssetLibrary.does_asset_exist(TEMPLATE_IA):
            w(u"！%s 不存在，没法复制出 IA_Interact。" % TEMPLATE_IA)
            return None
        ia = unreal.EditorAssetLibrary.duplicate_asset(TEMPLATE_IA, INTERACT_IA)
        w(u"  由 %s 复制出 IA_Interact" % TEMPLATE_IA)
    else:
        w(u"  IA_Interact 已存在")

    if ia is None:
        w(u"！IA_Interact 创建失败。")
        return None

    try:
        ia.set_editor_property("value_type", unreal.InputActionValueType.BOOLEAN)
        # 清掉模板带来的触发器/修改器：不带触发器就是最朴素的"按下即触发"，
        # 正好配 C++ 里绑的 ETriggerEvent::Started。
        ia.set_editor_property("triggers", [])
        ia.set_editor_property("modifiers", [])
    except Exception as exc:
        w(u"  设置 IA_Interact 属性时有项失败（大概率不影响按键）：%s" % exc)
    unreal.EditorAssetLibrary.save_loaded_asset(ia)

    imc = unreal.EditorAssetLibrary.load_asset(IMC_DEFAULT)
    if imc is None:
        w(u"！找不到 %s。" % IMC_DEFAULT)
        return ia

    key = _make_key(INTERACT_KEY)
    if key is None:
        w(u"！构造不出 FKey('%s')，请手动在 IMC_Default 里给 IA_Interact 映射 F。" % INTERACT_KEY)
        return ia
    try:
        # 先解绑再绑：重复跑脚本不会叠出一堆同样的映射。
        imc.unmap_key(ia, key)
    except Exception:
        pass
    imc.map_key(ia, key)
    unreal.EditorAssetLibrary.save_loaded_asset(imc)
    w(u"  IMC_Default：F → IA_Interact")
    return ia


def _make_key(name):
    for factory in (lambda: unreal.Key(name),
                    lambda: unreal.Key(key_name=name),
                    lambda: unreal.Key()):
        try:
            k = factory()
            if str(k.get_editor_property("key_name")) != name:
                k.set_editor_property("key_name", name)
            return k
        except Exception:
            continue
    return None


# ---------------------------------------------------------------- 3. 蓝图

def _mesh_bounds(mesh):
    """静态网格的包围盒，(min, max)。顶点是绝对坐标，所以这就是整车里的真实位置。"""
    try:
        box = mesh.get_bounding_box()
        return box.min, box.max
    except Exception:
        b = mesh.get_bounds()
        o, e = b.origin, b.box_extent
        return (unreal.Vector(o.x - e.x, o.y - e.y, o.z - e.z),
                unreal.Vector(o.x + e.x, o.y + e.y, o.z + e.z))


def _combined_bounds(meshes):
    lo = [1e18, 1e18, 1e18]
    hi = [-1e18, -1e18, -1e18]
    for m in meshes:
        mn, mx = _mesh_bounds(m)
        for i, (a, b) in enumerate(((mn.x, mx.x), (mn.y, mx.y), (mn.z, mx.z))):
            lo[i] = min(lo[i], a)
            hi[i] = max(hi[i], b)
    return lo, hi


def _warn_if_not_absolute(meshes):
    """所有部件的原点都挤在一起 = transform_vertex_to_absolute 没生效。

    这个失败会表现成"整车缩成一堆零件叠在同一个点上"，光看蓝图很难认出来是导入选项
    的问题，所以在这里点名。整车拼装完全依赖顶点留在场景绝对坐标里。
    """
    if len(meshes) < 3:
        return
    centers = []
    for m in meshes:
        mn, mx = _mesh_bounds(m)
        centers.append(((mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, (mn.z + mx.z) * 0.5))
    spread = max(
        max(abs(a[i] - b[i]) for i in range(3))
        for a in centers for b in centers)
    if spread < 10.0:
        w(u"！警告：%d 个车体部件的原点全挤在 %.1fcm 以内，说明导入时"
          u"「Transform Vertex to Absolute」没生效，整车会叠成一坨。" % (len(meshes), spread))
        w(u"  手动兜底：把 摩托车.fbx 拖进 Content/Vehicle/Motorbike/Parts，导入对话框里")
        w(u"  取消勾选 Combine Meshes、勾上 Transform Vertex to Absolute、Uniform Scale 填 %.2f，"
          u"再重跑 build_blueprint()。" % IMPORT_SCALE)


def build_blueprint(body=None, rider=None):
    body = body if body is not None else _collect_body_meshes()
    rider = rider if rider is not None else _load_rider()
    if not body:
        w(u"！没有车体网格，先跑 import_assets()。")
        return None

    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    if bp is None:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.DeliveryMotorbike)
        bp = _tools().create_asset(BP_NAME, DEST, unreal.Blueprint, factory)
        w(u"  新建 %s" % BP_PATH)
    else:
        w(u"  更新已有的 %s" % BP_PATH)
    if bp is None:
        w(u"！蓝图创建失败——C++ 编译过了吗（要有 unreal.DeliveryMotorbike）？")
        return None

    cdo = unreal.get_default_object(bp.generated_class())
    cdo.set_editor_property("body_meshes", body)

    _warn_if_not_absolute(body)

    lo, hi = _combined_bounds(body)
    size = [hi[i] - lo[i] for i in range(3)]
    center = [(hi[i] + lo[i]) * 0.5 for i in range(3)]
    w(u"  整车尺寸 %.0f x %.0f x %.0f cm（长/宽/高），中心 (%.0f, %.0f, %.0f)"
      % (size[0], size[1], size[2], center[0], center[1], center[2]))

    # 把整车挪到"包围盒中心落在 actor 原点"。于是车底正好在原点下方 高度/2 处，
    # HoverHeight 取同一个值，车轮就贴着地。
    mesh_root = cdo.get_editor_property("mesh_root")
    mesh_root.set_editor_property(
        "relative_location", unreal.Vector(-center[0], -center[1], -center[2]))

    hover = size[2] * 0.5
    cdo.set_editor_property("hover_height", hover)

    # 碰撞盒比车身矮 6cm：底面离地留一点缝，不然贴地的 sweep 会被地面刮到。
    box = cdo.get_editor_property("collision_box")
    box.set_editor_property("box_extent", unreal.Vector(
        max(size[0] * 0.5, 20.0), max(size[1] * 0.5, 15.0), max(size[2] * 0.5 - 6.0, 10.0)))

    if rider:
        rider_comp = cdo.get_editor_property("rider_mesh")
        for prop in ("skeletal_mesh_asset", "skeletal_mesh"):
            try:
                rider_comp.set_editor_property(prop, rider)
                break
            except Exception:
                continue
        w(u"  骑手网格已挂上（%s）" % rider.get_name())
    else:
        w(u"！没有骑手骨骼网格，车上不会有人。")

    try:
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    except Exception as exc:
        w(u"  编译蓝图时报错（多半无碍）：%s" % exc)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    return bp


# ---------------------------------------------------------------- 4. 放进关卡

def _ground_z(world, x, y, z):
    hit = unreal.SystemLibrary.line_trace_single(
        world, unreal.Vector(x, y, z + 2000.0), unreal.Vector(x, y, z - 5000.0),
        unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [], unreal.DrawDebugTrace.NONE, True)
    if hit is None:
        return None
    try:
        d = hit.to_dict()   # HitResult 的属性是 protected，只能走 to_dict
    except Exception:
        return None
    if d.get("blocking_hit") is False:
        return None
    point = d.get("impact_point") or d.get("location")
    return point.z if point else None


def place_in_level(bp=None):
    bp = bp or unreal.EditorAssetLibrary.load_asset(BP_PATH)
    if bp is None:
        w(u"！没有 BP_Motorbike，先跑 build_blueprint()。")
        return None

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    level_name = world.get_name() if world else "?"
    if EXPECTED_LEVEL.lower() not in level_name.lower():
        w(u"  注意：当前打开的是 %s，不是 %s。车会放进当前这张图。" % (level_name, EXPECTED_LEVEL))

    removed = 0
    for actor in eas.get_all_level_actors():
        if TAG in [str(t) for t in actor.tags]:
            eas.destroy_actor(actor)
            removed += 1
    if removed:
        w(u"  先删掉之前放的 %d 辆" % removed)

    origin = unreal.Vector(0.0, 0.0, 0.0)
    yaw = 0.0
    starts = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.PlayerStart)]
    if starts:
        start = starts[0]
        base = start.get_actor_location()
        yaw = start.get_actor_rotation().yaw
        forward = start.get_actor_forward_vector()
        origin = unreal.Vector(base.x + forward.x * SPAWN_AHEAD,
                               base.y + forward.y * SPAWN_AHEAD,
                               base.z)
        w(u"  以出生点 %s 前方 %.0fcm 为落点" % (start.get_actor_label(), SPAWN_AHEAD))
    else:
        w(u"  关卡里没有 PlayerStart，落点用世界原点。")

    hover = 60.0
    try:
        hover = float(unreal.get_default_object(bp.generated_class()).get_editor_property("hover_height"))
    except Exception:
        pass

    ground = _ground_z(world, origin.x, origin.y, origin.z)
    if ground is None:
        w(u"  落点打不到地面，Z 用出生点高度。")
        z = origin.z
    else:
        z = ground + hover + 5.0

    actor = eas.spawn_actor_from_class(
        bp.generated_class(), unreal.Vector(origin.x, origin.y, z), unreal.Rotator(0.0, yaw, 0.0))
    if actor is None:
        w(u"！放置失败。")
        return None
    actor.set_actor_label("Motorbike_Test")
    actor.tags = [TAG]
    w(u"  已放置 Motorbike_Test @ (%.0f, %.0f, %.0f)，朝向 %.0f 度" % (origin.x, origin.y, z, yaw))
    w(u"  关卡还没保存，自己 Ctrl+S。")
    return actor


# ---------------------------------------------------------------- 入口

def run():
    lines[:] = []
    w(u"=== 摩托车接入 ===")
    try:
        w(u"[1/4] 导入资产")
        body, rider = import_assets()
        w(u"[2/4] 输入")
        setup_input()
        w(u"[3/4] 蓝图")
        bp = build_blueprint(body, rider)
        w(u"[4/4] 放进关卡")
        if bp:
            place_in_level(bp)
        w("")
        w(u"完成。进 PIE 走到车边上，屏幕上会浮出「按 F 驾驶」，按 F 上车，")
        w(u"WASD 驾驶（W/S 油门刹车，A/D 转向），再按一次 F 下车。")
    except Exception:
        w(u"！出错了：")
        w(traceback.format_exc())
    flush()


if __name__ == "__main__":
    run()
