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

import math
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

# 骑手那三个网格按静态网格导进来是站姿的废品。**靠排除、不靠删除**：
# 第一版用 delete_asset 删，它对刚导入还没存盘的资产会静默失败（返回 False 不抛异常），
# 于是 12 个部件全进了 BodyMeshes，车上永远站着一个 T-pose 的人。
# 现在改成从 BodyMeshes 里过滤掉，删不删得掉都不影响结果。
RIDER_NAME_HINTS = ("character_body", "eye_left", "eye_right", "eyes")
# 名字对不上时的第二道判据：整份材质都是骑手材质的就是骑手部件。
# 车体用的是 材质*，骑手用的是 tripo_mat_* 和 Eyes_Black。
RIDER_MATERIAL_HINTS = ("tripo_mat", "eyes_black")

# 车头朝向修正。实测 UE 轴 = FBX 轴的这个映射：UE_X=FBX_X、UE_Y=FBX_Z、UE_Z=FBX_Y
# （用第一次导入报告里的 166x256x242 和 FBX 实测尺寸逐项对上的）。
# 车身长边 256cm 落在 UE 的 Y 上，而 Pawn 是朝 +X 开的，所以模型要转 -90 度才对得上。
# 车头是 +FBX_Z 这一侧 —— 依据是骑手的手（Z=34.1）在髋（Z=19.5）前面。
# None = 自动：长边在 Y 上就用 -90，否则 0。要强制就直接填 0/90/-90/180。
MESH_YAW_OVERRIDE = None

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
        # **必须为 True**，这是坐姿能不能进来的开关，别再关掉。
        #
        # 名字叫"用第 0 帧当参考姿势"，听起来像是给有动画的文件用的，第一版因此关掉了，
        # 结果导进来的骑手是站着的。看引擎源码 FbxSkeletalMeshImport.cpp:1291 才明白：
        #   * 关着  → GlobalsPerLink 取自 **BindPose**（这份 FBX 的绑定姿势是站姿）
        #   * 开着  → 用 GetNodeGlobalTransform(Link, 0) 覆盖，也就是**骨骼节点的当前变换**
        # 而这份 FBX 的坐姿正好写在节点当前变换里，所以要坐姿就必须开。
        # 没有动画不影响：t0 取的是节点变换，不需要 AnimStack。
        data.set_editor_property("use_t0_as_ref_pose", True)
        # 写完读回来确认一次。属性名拼错的话 set 会抛，但万一是别的原因没生效，
        # 光看"脚本跑过了"证明不了任何事（这个项目在"计数器证明不了结果留住了"上栽过）。
        w(u"  use_t0_as_ref_pose 读回 = %s" % data.get_editor_property("use_t0_as_ref_pose"))
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

    body = _collect_body_meshes()
    rider = _load_rider()
    w(u"  车体部件 %d 个，骑手 %s" % (len(body), u"有" if rider else u"没有（！）"))
    _save_imported()
    return body, rider


def _save_imported():
    """把导入产物真正写到盘上。第一次跑只存了蓝图，静态网格全是内存里的脏包。"""
    try:
        unreal.EditorAssetLibrary.save_directory(DEST, only_if_is_dirty=False, recursive=True)
        w(u"  资产已存盘")
    except Exception as exc:
        w(u"  存盘失败（自己 Ctrl+Shift+S 一下）：%s" % exc)


def _material_names(mesh):
    names = []
    try:
        for slot in mesh.get_editor_property("static_materials"):
            mi = slot.get_editor_property("material_interface")
            if mi:
                names.append(mi.get_name().lower())
    except Exception:
        pass
    return names


def _is_rider_part(mesh):
    """这个静态网格是不是骑手（站姿废品）。名字和材质两道判据，命中一条就算。"""
    name = mesh.get_name().lower()
    if any(hint in name for hint in RIDER_NAME_HINTS):
        return True
    mats = _material_names(mesh)
    return bool(mats) and all(
        any(hint in m for hint in RIDER_MATERIAL_HINTS) for m in mats)


def _collect_body_meshes():
    out = []
    if not unreal.EditorAssetLibrary.does_directory_exist(PARTS_DEST):
        return out
    for path in sorted(unreal.EditorAssetLibrary.list_assets(PARTS_DEST, recursive=False)):
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if not isinstance(asset, unreal.StaticMesh):
            continue
        # 名字和材质都打出来：万一两道判据都没拦住，看日志能一眼认出是谁混进来了。
        if _is_rider_part(asset):
            w(u"    跳过骑手静态副本 %s（材质 %s）"
              % (asset.get_name(), ",".join(_material_names(asset)) or u"无"))
            continue
        w(u"    车体部件 %s（材质 %s）"
          % (asset.get_name(), ",".join(_material_names(asset)) or u"无"))
        out.append(asset)
    return out


def _load_rider():
    asset = unreal.EditorAssetLibrary.load_asset(DEST + "/" + RIDER_ASSET_NAME)
    return asset if isinstance(asset, unreal.SkeletalMesh) else None


def _import_data_kind(asset):
    """这份资产是哪个导入器导进来的。**必须查**，不能假设。

    UE 5.8 里 FBX 有两条导入路：老的 FbxFactory（读 FbxImportUI 那一堆选项）和
    Interchange（完全不读）。实测**同样的任务，首次导入走老路、replace_existing
    重导却被路由到了 Interchange**，于是 use_t0_as_ref_pose 明明读回 True，
    却根本没人看——报告里一片正常，结果全错。
    看 AssetImportData 的类型就能分辨：Interchange 导的是 InterchangeAssetImportData。
    """
    try:
        data = asset.get_editor_property("asset_import_data")
        return type(data).__name__ if data else u"无"
    except Exception as exc:
        return u"读不到(%s)" % exc


def reimport_rider():
    """重导骑手，不动那 12 个车体静态网格。

    先删掉旧的骨骼网格 + 骨架再导，**不是** replace_existing 重导：
    带着已有资产做 replace_existing 会被路由到 Interchange，FbxImportUI 的选项
    （包括决定坐姿的 use_t0_as_ref_pose）会被整份忽略。干净导入才走老的 FbxFactory。
    """
    # 删之前先把蓝图里的引用摘掉，否则资产被引用着删不干净。
    bp = unreal.EditorAssetLibrary.load_asset(BP_PATH)
    cdo = unreal.get_default_object(bp.generated_class()) if bp else None
    if cdo:
        try:
            for prop in ("skinned_asset", "skeletal_mesh_asset", "skeletal_mesh"):
                try:
                    cdo.get_editor_property("rider_mesh").set_editor_property(prop, None)
                    break
                except Exception:
                    continue
        except Exception:
            pass

    for suffix in ("", "_Skeleton", "_PhysicsAsset"):
        path = DEST + "/" + RIDER_ASSET_NAME + suffix
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            ok = unreal.EditorAssetLibrary.delete_asset(path)
            w(u"  删除旧资产 %s%s" % (path, u"" if ok else u"（失败）"))

    sk_task = _make_task(DEST, True, False, RIDER_ASSET_NAME)
    _tools().import_asset_tasks([sk_task])
    paths = [str(p) for p in sk_task.get_editor_property("imported_object_paths")]
    w(u"重导骑手，产物 %d 个：%s" % (len(paths), ", ".join(paths[:4])))

    rider = _load_rider()
    if rider:
        kind = _import_data_kind(rider)
        w(u"  导入器：%s" % kind)
        if "Interchange" in kind:
            w(u"！又被 Interchange 接走了，FbxImportUI 的选项（含 use_t0_as_ref_pose）全部无效。")
            w(u"  这次的结果不作数，不用看下面的姿势判定。")
        # 引用被摘过，重新挂回去。
        if cdo:
            for prop in ("skinned_asset", "skeletal_mesh_asset", "skeletal_mesh"):
                try:
                    cdo.get_editor_property("rider_mesh").set_editor_property(prop, rider)
                    break
                except Exception:
                    continue
            try:
                unreal.BlueprintEditorLibrary.compile_blueprint(bp)
            except Exception:
                pass
            unreal.EditorAssetLibrary.save_loaded_asset(bp)
            w(u"  骑手已重新挂回 BP_Motorbike")

    _save_imported()

    body = _collect_body_meshes()
    center = None
    if body:
        lo, hi = _combined_bounds(body)
        center = [(hi[i] + lo[i]) * 0.5 for i in range(3)]
    _check_rider_is_seated(rider, center)
    flush()
    return rider


def _find_bone(comp, *needles):
    """按子串找骨骼名，不写死 `mixamorig:` 前缀（导入器有可能改名）。"""
    try:
        count = comp.get_num_bones()
    except Exception:
        return None
    for index in range(count):
        name = str(comp.get_bone_name(index))
        low = name.lower()
        if all(n in low for n in needles):
            return name
    return None


def _rider_thigh_angle(rider):
    """大腿和"竖直向下"的夹角，单位度。坐姿约 49°，站姿约 4°。

    这是判坐/站的正经判据：**和缩放、和包围盒余量都无关**，只看骨头指向哪。
    之前用包围盒高度拍阈值，坐姿实测 182cm、站姿 190cm，根本分不开，一直误报。
    临时 spawn 一个 SkeletalMeshActor 读参考姿势下的骨骼位置，读完就删。
    """
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = eas.spawn_actor_from_class(
        unreal.SkeletalMeshActor, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if actor is None:
        return None
    try:
        comp = actor.get_editor_property("skeletal_mesh_component")
        for prop in ("skeletal_mesh_asset", "skeletal_mesh"):
            try:
                comp.set_editor_property(prop, rider)
                break
            except Exception:
                continue

        hip = _find_bone(comp, "upleg")
        knee = _find_bone(comp, "leftleg") or _find_bone(comp, "leg")
        if not hip or not knee:
            return None
        a = comp.get_socket_location(hip)
        b = comp.get_socket_location(knee)
        dx, dy, dz = b.x - a.x, b.y - a.y, b.z - a.z
        length = math.sqrt(dx * dx + dy * dy + dz * dz)
        if length < 1e-3:
            return None
        # 和 (0,0,-1) 的夹角
        return math.degrees(math.acos(max(-1.0, min(1.0, -dz / length))))
    except Exception:
        return None
    finally:
        eas.destroy_actor(actor)


def _check_rider_is_seated(rider, bike_center=None):
    """验证骑手是不是真的坐着、而且坐在车上。把"看起来不对"变成两个可验证的数。

    ① 姿势：FBX 里骑手站立高 99.9 个单位，坐姿时头顶只到约 63 个单位
       （髋 16.45 / 头骨 52.45 量出来的）。差着近一倍，包围盒足够分得开，
       不需要去读参考骨架。
    ② 位置：骨骼网格的原点未必带上 Armature 节点在场景里的位移。带了的话骑手会
       正好落在车上；没带的话会偏出去几十厘米。拿它和车体包围盒中心比一下就知道。
    """
    if not rider:
        return
    try:
        bounds = rider.get_bounds()
        height = float(bounds.box_extent.z) * 2.0
        origin = bounds.origin
    except Exception as exc:
        w(u"  （量不到骑手包围盒，跳过坐姿检查：%s）" % exc)
        return

    w(u"  骑手包围盒高度 %.0fcm（仅供参考，坐姿和站姿只差 8cm 左右，分不开）" % height)

    angle = _rider_thigh_angle(rider)
    if angle is None:
        w(u"  （读不到骨骼，姿势判定跳过——自己在资产里看一眼）")
    elif angle >= 25.0:
        w(u"  姿势判定：坐姿 ✔（大腿偏离竖直 %.0f°，坐姿约 49°、站姿约 4°）" % angle)
    else:
        w(u"！骑手是**站着**的（大腿偏离竖直只有 %.0f°，坐姿应约 49°）。" % angle)
        w(u"  先查上面那行「导入器」：写着 Interchange 就是选项被忽略了，删掉资产重新干净导入；")
        w(u"  写着 FbxSkeletalMeshImportData 还这样，才轮到回 Blender 把坐姿 Apply as Rest Pose。")

    if bike_center is not None:
        offset = max(abs(float(origin.x) - bike_center[0]),
                     abs(float(origin.y) - bike_center[1]))
        w(u"  骑手中心 (%.0f, %.0f, %.0f)，车体中心 (%.0f, %.0f, %.0f)，水平偏差 %.0fcm"
          % (origin.x, origin.y, origin.z, bike_center[0], bike_center[1], bike_center[2], offset))
        if offset > 60.0:
            w(u"！骑手没坐在车上：骨骼网格的原点没带上 Armature 在场景里的位移。")
            w(u"  在 BP_Motorbike 的 RiderMesh 上手填一个相对位移补偿即可，车体不用动。")


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
    w(u"  ※ 记得提交 Content/Input/IMC_Default.uasset —— 这个映射被 git 拉取冲掉过一次，")
    w(u"    现象是浮窗照常显示、按 F 却毫无反应（浮窗不依赖按键绑定，所以很容易看岔）。")
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


def _pick_wheels(body):
    """认出轮子，返回 (下标列表, 轮心列表, 半径)。按几何认，不写死下标。

    判据两条，缺一不可：
      * **正圆**——长(Y)和高(Z)的比 > 0.9。轮子是唯一会满足这条的部件。
      * **窄**——宽(X) < 直径的 0.6。挡住的是车架/油箱那种"看着也挺方"的部件：
        实测 车.007 圆度 0.96 但宽 63.3 比直径 61.8 还大，一眼不是轮子。
    实测两个轮子都是 61.3 x 61.3、宽 27.7、圆度 1.00，第三名圆度 0.85，分得很开。

    半径按包围盒取，不按顶点——轮胎是凸的，包围盒就是直径。
    """
    if not body:
        return [], [], 30.0

    picked, centers, radii = [], [], []
    for index, mesh in enumerate(body):
        mn, mx = _mesh_bounds(mesh)
        w_x = float(mx.x - mn.x)
        l_y = float(mx.y - mn.y)
        h_z = float(mx.z - mn.z)
        diameter = max(l_y, h_z)
        if diameter <= 0.0:
            continue
        roundness = min(l_y, h_z) / diameter
        if roundness > 0.9 and w_x < diameter * 0.6:
            picked.append(index)
            centers.append(((float(mn.x) + float(mx.x)) * 0.5,
                            (float(mn.y) + float(mx.y)) * 0.5,
                            (float(mn.z) + float(mx.z)) * 0.5))
            radii.append(diameter * 0.5)
            w(u"  轮子：槽位 %d（直径 %.0fcm 宽 %.0fcm 圆度 %.2f 轮心 %.0f,%.0f,%.0f）"
              % (index, diameter, w_x, roundness, centers[-1][0], centers[-1][1], centers[-1][2]))

    if not picked:
        w(u"！没认出轮子，车轮不会转（不影响行驶）。")
        return [], [], 30.0

    radius = sum(radii) / len(radii)
    return picked, centers, radius


def _pick_steering_parts(body, lo, hi):
    """认出跟着龙头转的部件，返回 (下标列表, 转向轴位置)。按几何认，不写死下标。

    导入空间里车头朝 +Y（摆正前），所以：
      * **车把** = X 方向最宽的那个部件。实测它有 136cm 宽，第二名才 80cm，
        而且骑手两只手正好落在它上面（离线从 FBX 骨骼位置核对过），认得很稳。
      * **前轮 / 前叉** = 包围盒中心落在车身前四分之一的部件。
    车架横跨全车、中心在中部，不会被误抓；后轮、座、尾灯都在后半段。

    转向轴取"车把中心 和 最前部件中心"的水平中点——大致就是前叉的位置。
    用竖直轴而不是带后倾角的真实转向轴：差别在这个尺寸下看不出来，
    而竖直轴不需要再处理一层旋转补偿。
    """
    if not body:
        return [], (0.0, 0.0, 0.0)

    boxes = []
    for index, mesh in enumerate(body):
        mn, mx = _mesh_bounds(mesh)
        boxes.append({
            "i": index,
            "width": float(mx.x - mn.x),
            "cx": (float(mn.x) + float(mx.x)) * 0.5,
            "cy": (float(mn.y) + float(mx.y)) * 0.5,
            "cz": (float(mn.z) + float(mx.z)) * 0.5,
        })

    length = hi[1] - lo[1]
    front_line = lo[1] + length * 0.75

    bar = max(boxes, key=lambda b: b["width"])
    front = [b for b in boxes if b["cy"] >= front_line]

    # 车把单独一组：它只跟转一部分角度（骑手的手不会跟着走，转多了脱把）。
    full = sorted(b["i"] for b in front if b["i"] != bar["i"])

    front_most = max(boxes, key=lambda b: b["cy"])
    pivot = ((bar["cx"] + front_most["cx"]) * 0.5,
             (bar["cy"] + front_most["cy"]) * 0.5,
             bar["cz"])

    w(u"  打满跟转（前轮/前叉）：%s" % (",".join(str(i) for i in full) or u"无"))
    w(u"  部分跟转（车把）：%s（宽 %.0fcm）" % (bar["i"], bar["width"]))
    w(u"  转向轴 (%.0f, %.0f, %.0f)" % pivot)
    return full, [bar["i"]], pivot


def _rider_hips_location(rider):
    """骑手胯部在导入空间里的位置，骑手绕这根竖轴扭身。

    不用转向轴：绕车头那根轴转会把整个人往旁边甩（胯离轴心 70 多厘米），
    绕自己胯部转才是"扭身"，屁股留在座上、肩和手往车把那边跟一点。
    """
    if not rider:
        return None
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = eas.spawn_actor_from_class(
        unreal.SkeletalMeshActor, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if actor is None:
        return None
    try:
        comp = actor.get_editor_property("skeletal_mesh_component")
        for prop in ("skeletal_mesh_asset", "skeletal_mesh"):
            try:
                comp.set_editor_property(prop, rider)
                break
            except Exception:
                continue
        hips = _find_bone(comp, "hips")
        if not hips:
            return None
        loc = comp.get_socket_location(hips)
        return (float(loc.x), float(loc.y), float(loc.z))
    except Exception:
        return None
    finally:
        eas.destroy_actor(actor)


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
    w(u"  导入空间下的整车包围盒 %.0f x %.0f x %.0f cm，中心 (%.0f, %.0f, %.0f)"
      % (size[0], size[1], size[2], center[0], center[1], center[2]))

    yaw = MESH_YAW_OVERRIDE
    if yaw is None:
        # 长边不在 X 上就说明车头没朝 +X。车头是 +Y 那一侧（见常量处的推导），转 -90 归位。
        yaw = -90.0 if size[1] > size[0] else 0.0
    w(u"  车头朝向修正 %.0f 度%s" % (yaw, u"（自动判定）" if MESH_YAW_OVERRIDE is None else u"（手动指定）"))

    # 旋转之后再算 actor 空间下的尺寸和居中偏移。
    # 组件变换是"先转再平移"，所以要把中心点也转过去再取反，否则转完就偏出去了。
    rot = math.radians(yaw)
    cos_y, sin_y = math.cos(rot), math.sin(rot)
    rotated_center = (center[0] * cos_y - center[1] * sin_y,
                      center[0] * sin_y + center[1] * cos_y,
                      center[2])
    if abs(size[0] * cos_y) < abs(size[1] * sin_y):
        actor_size = [size[1], size[0], size[2]]
    else:
        actor_size = [size[0], size[1], size[2]]
    w(u"  摆正后：长 %.0f 宽 %.0f 高 %.0f cm" % (actor_size[0], actor_size[1], actor_size[2]))

    # MeshRoot 只管侧倾，位置必须清零。
    # 早一版把居中偏移写在 MeshRoot 上，那个覆盖会留在已有的蓝图里，
    # 和 MeshAlign 的偏移叠起来会把整车推出去一倍距离。
    mesh_root = cdo.get_editor_property("mesh_root")
    mesh_root.set_editor_property("relative_location", unreal.Vector(0.0, 0.0, 0.0))
    mesh_root.set_editor_property("relative_rotation", unreal.Rotator(0.0, 0.0, 0.0))

    # 朝向和居中都落在 MeshAlign 上。
    #
    # 注意 unreal.Rotator 的构造参数是 (roll, pitch, yaw)，**和 C++ 的 FRotator(Pitch, Yaw, Roll)
    # 顺序不一样**。把 yaw 填进第二个位置会变成 pitch，车会被竖起来立在车头上
    # ——这个坑已经踩过一次，别再改回去。参照 spawn_test_cars.py 里的写法。
    mesh_align = cdo.get_editor_property("mesh_align")
    mesh_align.set_editor_property("relative_rotation", unreal.Rotator(0.0, 0.0, yaw))
    mesh_align.set_editor_property(
        "relative_location",
        unreal.Vector(-rotated_center[0], -rotated_center[1], -rotated_center[2]))

    wheel_indices, wheel_centers, wheel_radius = _pick_wheels(body)
    cdo.set_editor_property("wheel_part_indices", wheel_indices)
    cdo.set_editor_property("wheel_centers",
                            [unreal.Vector(c[0], c[1], c[2]) for c in wheel_centers])
    cdo.set_editor_property("wheel_radius", wheel_radius)

    steer_indices, bar_indices, pivot = _pick_steering_parts(body, lo, hi)
    cdo.set_editor_property("steering_part_indices", steer_indices)
    cdo.set_editor_property("handlebar_part_indices", bar_indices)
    cdo.set_editor_property("steer_pivot_location", unreal.Vector(pivot[0], pivot[1], pivot[2]))

    hips = _rider_hips_location(rider)
    if hips:
        cdo.set_editor_property("rider_pivot_location", unreal.Vector(hips[0], hips[1], hips[2]))
        w(u"  骑手扭身轴（胯部）(%.0f, %.0f, %.0f)" % hips)
    else:
        w(u"  （读不到 hips 骨骼，骑手扭身轴留在原点——骑手会绕车身中心转，不自然但不致命）")

    hover = actor_size[2] * 0.5
    cdo.set_editor_property("hover_height", hover)

    # 碰撞盒比车身矮 6cm：底面离地留一点缝，不然贴地的 sweep 会被地面刮到。
    box = cdo.get_editor_property("collision_box")
    box.set_editor_property("box_extent", unreal.Vector(
        max(actor_size[0] * 0.5, 20.0),
        max(actor_size[1] * 0.5, 15.0),
        max(actor_size[2] * 0.5 - 6.0, 10.0)))

    _check_rider_is_seated(rider, center)

    if rider:
        rider_comp = cdo.get_editor_property("rider_mesh")
        # RiderMesh 是 PoseableMeshComponent（USkinnedMeshComponent），网格属性叫 skinned_asset；
        # 后两个是它在旧版本/SkeletalMeshComponent 上的名字，留着当兜底。
        for prop in ("skinned_asset", "skeletal_mesh_asset", "skeletal_mesh"):
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

    # 重跑时保留上一辆的位置朝向：摆过一次之后再跑脚本（改参数、换网格）
    # 不该把车弹回出生点，不然每次调完参数都要重新找地方摆。
    kept = None
    removed = 0
    for actor in eas.get_all_level_actors():
        if TAG in [str(t) for t in actor.tags]:
            if kept is None:
                kept = (actor.get_actor_location(), actor.get_actor_rotation())
            eas.destroy_actor(actor)
            removed += 1
    if removed:
        w(u"  先删掉之前放的 %d 辆（沿用它的位置朝向）" % removed)

    hover = 60.0
    try:
        hover = float(unreal.get_default_object(bp.generated_class()).get_editor_property("hover_height"))
    except Exception:
        pass

    if kept is not None:
        loc, rot = kept
        # 只留 yaw：车永远是正着立在地上的，俯仰和侧倾都该是 0。
        # 上一辆要是被放歪了（比如之前那个 Rotator 参数顺序的 bug），别把歪的姿态继承下来。
        rot = unreal.Rotator(0.0, 0.0, rot.yaw)
        actor = eas.spawn_actor_from_class(bp.generated_class(), loc, rot)
        if actor is None:
            w(u"！放置失败。")
            return None
        actor.set_actor_label("Motorbike_Test")
        actor.tags = [TAG]
        w(u"  已放回原位 (%.0f, %.0f, %.0f)" % (loc.x, loc.y, loc.z))
        w(u"  关卡还没保存，自己 Ctrl+S。")
        return actor

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
        # testfortraffic 里没有 PlayerStart。放世界原点等于扔进虚空里让人自己找，
        # 改成放在编辑器视口镜头前方——你现在看着哪儿，车就出现在哪儿。
        try:
            ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
            cam_loc, cam_rot = ues.get_level_viewport_camera_info()
            yaw = cam_rot.yaw
            fwd = unreal.MathLibrary.get_forward_vector(cam_rot)
            origin = unreal.Vector(cam_loc.x + fwd.x * 600.0,
                                   cam_loc.y + fwd.y * 600.0,
                                   cam_loc.z)
            w(u"  关卡里没有 PlayerStart，落点取编辑器视口镜头前方 600cm。")
        except Exception as exc:
            w(u"  关卡里没有 PlayerStart，也取不到视口镜头（%s），落点用世界原点。" % exc)

    ground = _ground_z(world, origin.x, origin.y, origin.z)
    if ground is None:
        w(u"  落点打不到地面，Z 用参考点高度。")
        z = origin.z
    else:
        z = ground + hover + 5.0

    actor = eas.spawn_actor_from_class(
        bp.generated_class(), unreal.Vector(origin.x, origin.y, z), unreal.Rotator(0.0, 0.0, yaw))
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
