"""二狗普通斧头的独立静态资源步骤，导入本文件不执行

main('inspect')：读实际模板、正式地图实例、作者动画及 LOD0 几何，只写 Saved
main('retarget')：只创建独立 A_ErgouAxeIdle，不修改原骨架、旧动画或地图
main('configure', manifest_path=..., manifest_sha256=...)：依据已复核的静态证据写五个明确字段
main('verify', manifest_path=..., manifest_sha256=...)：独立进程读回默认值和地图实例
必须在项目编辑器关闭后使用独立 NullRHI 静态窗口，本脚本不启动 UE 或执行游戏
"""
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re


BP = '/Game/Blueprint/Item/BP_Axe'
MAP = '/Game/Level/TestForCharacter'
NPC = '/Game/NPC/Companion/BP_Companion_Ergou'
MESH = '/Game/model/futou'
TARGET_MESH = '/Game/NPC/Companion/Appearance/SK_ErgouBoy'
TARGET_SKELETON = '/Game/Characters/NPC/SK_NpcMan_Skeleton'
SOURCE = '/Game/NPC/Companion/Home/Source/QuaterniusAligned/Library1/Quaternius_AnimationLibrary_StandardSword_Idle'
SOURCE_MESH = '/Game/NPC/Companion/Home/Source/QuaterniusAligned/Library1/Quaternius_AnimationLibrary_Standard'
RIG = '/Game/NPC/Companion/Home/Retarget/Aligned/RTG_HomeLibrary1ToErgou'
SOURCE_RIG = '/Game/NPC/Companion/Home/Retarget/Aligned/IK_HomeLibrary1'
TARGET_RIG = '/Game/NPC/Companion/Home/Retarget/Aligned/IK_ErgouHome'
OUTPUT = '/Game/NPC/Companion/Animation/A_ErgouAxeIdle'
CANDIDATE = '/Game/NPC/Companion/Animation/Candidates/AxeIdle/A_ErgouAxeIdle'
TAG = 'DeliveryCompanionAxeIdle'
HELPER = 'Content/Python/import_companion_home_animations.py'
HELPER_SHA = 'd8f4d7dfb9025d5463ddcb16f8c73540021cce26632623a924a5d3b8aca4825c'
PROCESS_HELPER = 'Content/Python/delivery_editor_process_guard.py'
PROCESS_HELPER_SHA = '749b055e9e2fca13afdfd9608647732f230ad1e8e4f405b2f735f6c7e91c63f2'
SOURCE_SHA = '1b7bf67866360665426bb99e4c71bd619f19b408453c24e30f0c3071601eee5c'
FRACTIONS = [0., .25, .5, .75, 1.]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def bound_json(path, expected):
    if not isinstance(expected, str) or len(expected) != 64 or sha(path) != expected.lower():
        raise RuntimeError('证据 SHA 不匹配：'+str(path))
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def module(path, name, expected):
    if sha(path) != expected:
        raise RuntimeError('静态资源辅助脚本已变，先重新审查：'+str(path))
    spec = importlib.util.spec_from_file_location(name, str(path))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def vector(value):
    result = [float(value.x), float(value.y), float(value.z)]
    if not all(math.isfinite(x) for x in result):
        raise RuntimeError('几何或配置包含非有限数值')
    return result


def transform(value):
    result = {'position': vector(value.translation), 'scale': vector(value.scale3d),
              'quaternion_xyzw': [float(value.rotation.x), float(value.rotation.y), float(value.rotation.z), float(value.rotation.w)]}
    if not all(math.isfinite(x) for values in result.values() for x in values):
        raise RuntimeError('变换包含非有限数值')
    return result


def main(mode='inspect', *, manifest_path=None, manifest_sha256=None):
    import unreal
    if mode not in ('inspect', 'retarget', 'configure', 'verify'):
        raise RuntimeError('未知静态步骤')
    project = Path(unreal.Paths.project_dir()).resolve()
    out = project/'Saved/AIResearch/CompanionItems/AxeAssets'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    out.mkdir(parents=True, exist_ok=False)
    report = {'mode': mode, 'status': 'started', 'script_sha256': sha(__file__),
              'not_run': ['PIE', 'Simulate', 'BeginPlay', 'WorldTick', 'model', 'TTS'],
              'map_saved': False, 'procedural_bone_tracks': False, 'visual_gameplay_accepted': False}
    path = lambda obj: obj.get_path_name() if obj else None
    package = lambda value: value.split('.', 1)[0] if value else None
    helper = None
    processes = None
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    util = unreal.EditorLoadingAndSavingUtils

    def guard(allowed_dirty=()):
        command = unreal.SystemLibrary.get_command_line().lower()
        if '-run=pythonscript' not in command or '-nullrhi' not in command:
            raise RuntimeError('只允许已分配窗口的独立 NullRHI Python Commandlet')
        processes.assert_exclusive_process()
        if not editor or editor.get_game_world():
            raise RuntimeError('游戏世界存在，停止资源操作')
        dirty_maps = [path(p) for p in util.get_dirty_map_packages()]
        dirty_assets = [path(p) for p in util.get_dirty_content_packages()]
        if dirty_maps or any(p not in allowed_dirty for p in dirty_assets):
            raise RuntimeError('存在范围外未保存改动：'+str(dirty_maps+dirty_assets))

    def finish_assets():
        if not unreal.DeliveryHomeObjectAssetTools.finish_pending_editor_assets():
            raise RuntimeError('异步依赖加载或编译尚未完成')

    def load(value, cls=None):
        obj = unreal.load_asset(value)
        if not obj or (cls and not isinstance(obj, cls)):
            raise RuntimeError('缺少或类型不符：'+value)
        return obj

    def item_snapshot(actor):
        mesh = actor.get_editor_property('item_mesh')
        item = actor.get_editor_property('item_component')
        interact = actor.get_editor_property('interactable_component')
        rotation = actor.get_editor_property('npc_held_rotation')
        fields = {'npc_hand_socket': str(actor.get_editor_property('npc_hand_socket')),
                  'npc_held_offset': vector(actor.get_editor_property('npc_held_offset')),
                  'npc_held_rotation': [float(rotation.pitch), float(rotation.yaw), float(rotation.roll)],
                  'npc_carry_animation': path(actor.get_editor_property('npc_carry_animation')),
                  'companion_can_fetch': bool(item.get_editor_property('companion_can_fetch'))}
        preserved = {'class': path(actor.get_class()), 'transform': transform(actor.get_actor_transform()),
            'mesh': path(mesh.get_editor_property('static_mesh')), 'mesh_relative': transform(mesh.get_relative_transform()),
            'materials': [path(mesh.get_material(i)) for i in range(mesh.get_num_materials())],
            'overrides': [path(x) for x in mesh.get_editor_property('override_materials')],
            'collision_profile': str(mesh.get_collision_profile_name()), 'collision_enabled': str(mesh.get_collision_enabled()),
            'player_socket': str(actor.get_editor_property('hand_socket')),
            'player_offset': transform(actor.get_editor_property('held_relative_transform')),
            'uses_axe_swing': bool(actor.get_editor_property('uses_axe_swing')),
            'item_type': str(item.get_editor_property('item_type')), 'display_name': str(item.get_editor_property('display_name')),
            'durability': item.get_editor_property('durability'), 'max_durability': item.get_editor_property('max_durability'),
            'prompt': str(interact.get_editor_property('prompt_text')), 'radius': interact.get_editor_property('interact_radius'),
            'components': sorted([x.get_name(), path(x.get_class())] for x in actor.get_components_by_class(unreal.ActorComponent))}
        return {'path': path(actor), 'fields': fields, 'preserved': preserved}

    def pose_samples(clip, mesh):
        options = unreal.AnimPoseEvaluationOptions()
        options.set_editor_property('evaluation_type', unreal.AnimDataEvalType.COMPRESSED)
        options.set_editor_property('optional_skeletal_mesh', mesh)
        samples = []
        for fraction in FRACTIONS:
            pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(clip, clip.get_play_length()*fraction, options)
            names = {str(n) for n in unreal.AnimPoseExtensions.get_bone_names(pose)}
            required = {'RightForeArm', 'RightHand', 'RightHandEnd', 'LeftHand', 'Spine2', 'Hips'}
            if not required.issubset(names):
                raise RuntimeError('缺少实际握持骨：'+str(required-names))
            samples.append({'fraction': fraction, 'time': clip.get_play_length()*fraction,
                            'mesh_component_bones': {n: transform(unreal.AnimPoseExtensions.get_bone_pose(pose, n, unreal.AnimPoseSpaces.WORLD)) for n in sorted(required)}})
        return samples

    def export_mesh(mesh):
        file = out/'futou_lod0.obj'
        task = unreal.AssetExportTask()
        task.object = mesh; task.exporter = unreal.StaticMeshExporterOBJ()
        task.filename = str(file); task.automated = True; task.prompt = False
        task.replace_identical = False; task.use_file_archive = False
        success = unreal.Exporter.run_asset_export_task(task)
        if not success or list(task.errors) or not file.is_file():
            raise RuntimeError('实际斧头 LOD0 导出失败：'+str(list(task.errors)))
        vertices, faces = [], []
        for line in file.read_text(encoding='utf-8-sig').splitlines():
            if line.startswith('v '):
                values = [float(x) for x in line.split()[1:]]
                if len(values) != 3 or not all(math.isfinite(x) for x in values):
                    raise RuntimeError('OBJ 顶点无效')
                vertices.append([values[0], values[2], values[1]])
            elif line.startswith('f '):
                indices = [int(x.split('/')[0])-1 for x in line.split()[1:]]
                if len(indices) != 3:
                    raise RuntimeError('预期 LOD0 三角形')
                faces.append(indices)
        if not vertices or not faces or any(i < 0 or i >= len(vertices) for f in faces for i in f):
            raise RuntimeError('OBJ 没有完整有效三角网格')
        geometry = {'mesh': path(mesh), 'mesh_sha256': sha(helper.asset_file(project, MESH)), 'obj': str(file), 'obj_sha256': sha(file),
                    'coordinates': 'UE static mesh local centimeters; restored OBJ (X,Z,Y) to UE (X,Y,Z)',
                    'vertices': vertices, 'triangles': faces}
        geometry_file = out/'futou_lod0_ue.json'
        geometry_file.write_text(json.dumps(geometry, ensure_ascii=False), encoding='utf-8')
        return {'path': str(geometry_file), 'sha256': sha(geometry_file), 'vertex_count': len(vertices), 'triangle_count': len(faces)}

    protected = {}
    try:
        helper = module(project/HELPER, 'delivery_axe_native_retarget', HELPER_SHA)
        processes = module(project/PROCESS_HELPER, 'delivery_axe_process_guard', PROCESS_HELPER_SHA)
        guard()
        protected_paths = [project/'Content/Level/TestForCharacter.umap', helper.asset_file(project, MESH),
            helper.asset_file(project, BP), helper.asset_file(project, NPC)]
        for folder in ('NPC/Companion/Appearance', 'NPC/Companion/Animation', 'NPC/Companion/Home/Source',
                       'NPC/Companion/Home/Retarget', 'Characters/NPC'):
            protected_paths.extend((project/'Content'/folder).rglob('*.uasset'))
        writable = {helper.asset_file(project, BP)} if mode == 'configure' else {helper.asset_file(project, OUTPUT), helper.asset_file(project, CANDIDATE)} if mode == 'retarget' else set()
        protected = {str(p): sha(p) for p in protected_paths if p.is_file() and p not in writable}
        bp = load(BP, unreal.Blueprint)
        cdo = unreal.get_default_object(bp.generated_class())
        if not isinstance(cdo, unreal.DeliveryHandheldItem) or isinstance(cdo, unreal.DeliveryPackageItem):
            raise RuntimeError('正式斧头不是普通手持物')
        target, idle = helper.target_data()
        if package(path(target)) != TARGET_MESH or package(path(target.get_editor_property('skeleton'))) != TARGET_SKELETON:
            raise RuntimeError('二狗实际模型或骨架已经变化')
        npc = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(NPC))
        body = npc.get_editor_property('mesh')
        if not body.does_socket_exist('RightHand'):
            raise RuntimeError('二狗没有实际 RightHand 骨或插槽')
        anchor = npc.get_editor_property('handheld_anchor')
        if not anchor or not anchor.get_editor_property('absolute_scale'):
            raise RuntimeError('二狗实际模板没有不继承骨缩放的普通手持挂点')
        info = helper.local_sources(project)['Library1']
        if info['sha256'] != SOURCE_SHA:
            raise RuntimeError('作者原包 SHA 已变化')
        source = load(SOURCE, unreal.AnimSequence)
        source_mesh = load(SOURCE_MESH, unreal.SkeletalMesh)
        retargeter = load(RIG)
        source_rig, target_rig = load(SOURCE_RIG), load(TARGET_RIG)
        controller = unreal.IKRetargeterController.get_controller(retargeter)
        if (controller.get_ik_rig(unreal.RetargetSourceOrTarget.SOURCE) != source_rig or
            controller.get_ik_rig(unreal.RetargetSourceOrTarget.TARGET) != target_rig or
            unreal.IKRigController.get_controller(source_rig).get_skeletal_mesh() != source_mesh or
            unreal.IKRigController.get_controller(target_rig).get_skeletal_mesh() != target):
            raise RuntimeError('已校验 IK 接线已变化')
        for asset in (source, source_mesh):
            if (unreal.EditorAssetLibrary.get_metadata_tag(asset, 'SourceSHA256') != SOURCE_SHA or
                unreal.EditorAssetLibrary.get_metadata_tag(asset, 'SourceBasisAlignment') != helper.ALIGNMENT_VERSION):
                raise RuntimeError('独立对齐源身份不符')
        finish_assets()
        report.update(cdo_before=item_snapshot(cdo), source=path(source), target=path(target),
                      source_file=info, actual_mesh_transform=transform(body.get_relative_transform()),
                      hand_anchor={'socket': str(anchor.get_attach_socket_name()), 'absolute_scale': True,
                                   'relative_transform': transform(anchor.get_relative_transform())},
                      body_graph=json.loads(unreal.DeliveryCompanionAnimationAssetTools.inspect_companion_body_animation()))
        if not report['body_graph'].get('success'):
            raise RuntimeError('实际独立动画图检查未通过')
        report['backup'] = helper.backup(project, out, [BP] if mode == 'configure' else [CANDIDATE, OUTPUT] if mode == 'retarget' else [])
        if mode == 'retarget':
            for destination in (CANDIDATE, OUTPUT):
                clip = unreal.load_asset(destination)
                if clip:
                    if (not isinstance(clip, unreal.AnimSequence) or
                        unreal.EditorAssetLibrary.get_metadata_tag(clip, TAG) != '1' or
                        unreal.EditorAssetLibrary.get_metadata_tag(clip, 'SourceAnimation') != path(source) or
                        unreal.EditorAssetLibrary.get_metadata_tag(clip, 'SourceSHA256') != SOURCE_SHA):
                        raise RuntimeError('已有不同动画，禁止覆盖：'+destination)
                else:
                    guard()
                    folder, name = destination.rsplit('/', 1)
                    clip = helper.run_native_retarget(source, source_mesh, target, retargeter, folder, name)
                    helper.set_retarget_metadata(clip, source, info, 'Sword_Idle', retargeter, True)
                    unreal.EditorAssetLibrary.set_metadata_tag(clip, TAG, '1')
                finish_assets()
                if clip.get_editor_property('skeleton') != target.get_editor_property('skeleton') or clip.get_editor_property('enable_root_motion'):
                    raise RuntimeError('目标骨架或 Root Motion 不符')
                direction = helper.verify_retarget_direction('AxeIdle', source, source_mesh, clip, target, 'Library1')
                report.setdefault('retarget', {})[destination] = {'direction': direction, 'samples': pose_samples(clip, target)}
                guard((destination,))
                helper.save_asset(clip)
            report['status'] = 'retargeted_single_authored_idle_needs_grip_review'
        else:
            clip = unreal.load_asset(OUTPUT)
            if clip:
                if clip.get_editor_property('skeleton') != target.get_editor_property('skeleton') or clip.get_editor_property('enable_root_motion'):
                    raise RuntimeError('持斧动作骨架或 Root Motion 不符')
                report['axe_idle_samples'] = pose_samples(clip, target)
                report['axe_idle_sha256'] = sha(helper.asset_file(project, OUTPUT))
            if mode in ('inspect', 'configure', 'verify'):
                world = util.load_map(MAP)
                finish_assets()
                if not world or editor.get_game_world():
                    raise RuntimeError('未能加载静态正式地图')
                instances = [a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
                             if a.get_class() == bp.generated_class() and a.get_actor_label() == 'OldManHouse_Axe']
                if len(instances) != 1:
                    raise RuntimeError('正式 OldManHouse_Axe 不唯一')
                report['placed_axe'] = item_snapshot(instances[0])
                if package(report['placed_axe']['preserved']['mesh']) != MESH:
                    raise RuntimeError('正式斧头模型不同')
                if mode == 'inspect':
                    report['geometry'] = export_mesh(load(MESH, unreal.StaticMesh))
                    report['status'] = 'inspected_needs_actual_grip_review'
            if mode in ('configure', 'verify'):
                manifest = bound_json(manifest_path, manifest_sha256)
                evidence = bound_json(manifest['static_review']['path'], manifest['static_review']['sha256'])
                if (manifest.get('schema') != 1 or not clip or manifest['actor_blueprint'] != BP or
                    manifest['animation'] != OUTPUT or manifest['actor_scale_policy'] != 'preserve_original' or
                    evidence.get('status') != 'reviewed_static_grip' or evidence.get('sample_fractions') != FRACTIONS or
                    evidence.get('fields') != manifest['fields'] or evidence.get('animation_sha256') != sha(helper.asset_file(project, OUTPUT)) or
                    evidence.get('mesh_sha256') != sha(helper.asset_file(project, MESH)) or
                    not evidence.get('geometry_report') or not evidence.get('render_files')):
                    raise RuntimeError('缺少绑定当前网格/动画/握点及五时刻画面的静态复核证据')
                geometry = bound_json(evidence['geometry_report']['path'], evidence['geometry_report']['sha256'])
                if (package(geometry.get('mesh')) != MESH or geometry.get('mesh_sha256') != sha(helper.asset_file(project, MESH)) or
                    evidence.get('actor_scale') != manifest['before_placed_axe']['preserved']['transform']['scale']):
                    raise RuntimeError('握点几何或实际物件尺寸不符')
                for rendered in evidence['render_files']:
                    file = Path(rendered['path']).resolve()
                    if not file.is_relative_to((project/'Saved').resolve()) or not file.is_file() or sha(file) != rendered['sha256']:
                        raise RuntimeError('静态画面文件未找到或已变化')
                fields = manifest['fields']
                if set(fields) != {'npc_hand_socket', 'npc_held_offset', 'npc_held_rotation', 'npc_carry_animation', 'companion_can_fetch'}:
                    raise RuntimeError('配置只能包含五个明确字段')
                for key in ('npc_held_offset', 'npc_held_rotation'):
                    if len(fields[key]) != 3 or not all(isinstance(v, (int, float)) and math.isfinite(v) for v in fields[key]):
                        raise RuntimeError('尚未实测的握持配置不得写入')
                if fields['npc_hand_socket'] != 'RightHand' or package(fields['npc_carry_animation']) != OUTPUT or type(fields['companion_can_fetch']) is not bool:
                    raise RuntimeError('握持骨/动画/取物开关不符')
                if mode == 'configure':
                    if sha(helper.asset_file(project, BP)) != manifest['before_blueprint_sha256'] or report['cdo_before'] != manifest['before_cdo']:
                        raise RuntimeError('斧头默认值在标定后已变化')
                    if report['placed_axe'] != manifest['before_placed_axe']:
                        raise RuntimeError('关卡斧头的位置、尺寸或默认值在标定后已变化')
                    # 实例字段有单独覆盖时，写蓝图默认值不能保证场景物件跟着更新
                    if report['placed_axe']['fields'] != report['cdo_before']['fields']:
                        raise RuntimeError('关卡斧头有独立的同伴握持配置，先处理实例覆盖')
                    guard()
                    bp.modify()
                    cdo.set_editor_property('npc_hand_socket', unreal.Name(fields['npc_hand_socket']))
                    cdo.set_editor_property('npc_held_offset', unreal.Vector(*fields['npc_held_offset']))
                    p, y, r = fields['npc_held_rotation']
                    cdo.set_editor_property('npc_held_rotation', unreal.Rotator(pitch=p, yaw=y, roll=r))
                    cdo.set_editor_property('npc_carry_animation', clip)
                    cdo.get_editor_property('item_component').set_editor_property('companion_can_fetch', fields['companion_can_fetch'])
                    if not unreal.BlueprintEditorLibrary.compile_blueprint(bp):
                        raise RuntimeError('斧头蓝图编译失败，未保存')
                    cdo = unreal.get_default_object(bp.generated_class())
                    after = item_snapshot(cdo)
                    if after['fields'] != fields or after['preserved'] != report['cdo_before']['preserved']:
                        raise RuntimeError('读回出现范围外变化，未保存')
                    guard((BP,))
                    helper.save_asset(bp)
                    report.update(cdo_after=after, saved_blueprint_sha256=sha(helper.asset_file(project, BP)), status='configured_needs_independent_reload_and_playtest')
                else:
                    if report['cdo_before']['fields'] != fields or report['placed_axe']['fields'] != fields:
                        raise RuntimeError('独立重载默认值或实例尚未使用本次配置')
                    if report['cdo_before']['preserved'] != manifest['before_cdo']['preserved'] or report['placed_axe']['preserved'] != manifest['before_placed_axe']['preserved']:
                        raise RuntimeError('独立重载发现玩家配置/尺寸/模型等保护字段变化')
                    report['status'] = 'verified_saved_configuration_not_gameplay'
        guard()
    except Exception as error:
        report.update(status='failed', error=repr(error))
        raise
    finally:
        current = {p: sha(p) if Path(p).is_file() else None for p in protected}
        report.update(protected_before=protected, protected_after=current, protected_unchanged=current == protected,
                      limits=['Sword_Idle is authored; no dedicated authored one-hand tool walk is present in the two downloaded libraries',
                              'AnimPose WORLD is mesh component space; this is not a live evaluated upper-body graph or a game acceptance',
                              'No player hand configuration, package anchor, bike cargo, axe size, damage or collision is changed'])
        (out/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        unreal.log('COMPANION_AXE_ASSET_REPORT='+str(out/'report.json'))
        if current != protected:
            raise RuntimeError('受保护网格、骨架、旧动画、蓝图或地图发生变化')
    return str(out/'report.json')


if __name__ == '__main__':
    import unreal
    command = unreal.SystemLibrary.get_command_line()
    def argument(name):
        found = re.search(r'(?:^|\s)-'+re.escape(name)+r'=(?:"([^"]+)"|(\S+))', command, re.IGNORECASE)
        return (found.group(1) or found.group(2)) if found else None
    stage = argument('NpcAxeStage')
    if not stage:
        raise RuntimeError('请明确指定 -NpcAxeStage=inspect/retarget/configure/verify')
    main(stage.lower(), manifest_path=argument('NpcAxeManifest'), manifest_sha256=argument('NpcAxeManifestSHA256'))
