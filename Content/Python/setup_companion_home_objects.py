"""复用现有房屋家具，默认只读，不启动 PIE 或调用游戏逻辑

-InspectCompanionHomeObjects：只读真实家具与动画基准，输出标定 manifest 草案
-SetupCompanionHomeObjects -HomeObjectsManifest="..."：显式备份后创建资源并替换指定家具
-ReplaceCompanionHomeAssets：允许重建本工具创建的资源；未知资产仍拒绝覆盖

Setup 要求 manifest.calibrated=true，不能把包围盒推算当作座位、房间或摆放验收
坐标：slot 相对家具原点（受家具 Actor scale 影响）；entrance/use/exit 相对槽位、单位 cm
Sit 的 use 是角色脚下基准；PlaceProp 的 use 是支撑面中心，服务器按物件原尺寸计算 Actor 原点
PlaceProp 的道具中转点相对槽位，单位为世界厘米，不继承家具缩放，默认关闭
动画只引用已导入的成品动作；本脚本不生成或修改动画、骨架或骨轨道
Inspect 的 idle_bones_mesh_cm 是网格厘米坐标，不再乘导入比例
脚踝骨骼位置不等于鞋底，座面高度与角色脚下基准须在静态预览中一起标定
"""
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import uuid

import unreal


MAP = '/Game/Level/TestForCharacter'
ROOT = '/Game/NPC/Companion/Home'
NPC = '/Game/NPC/Companion/BP_Companion_Ergou'
ANIM_ROOT = '/Game/NPC/Companion/Animation'
BODY_BLUEPRINT = ANIM_ROOT + '/ABP_CompanionBody'
CARRY_EVIDENCE = 'Saved/AIResearch/HomeObjects/CarryAnchors/20261114T105816880682Z/analysis.json'
ANIMATIONS = {name: ANIM_ROOT + '/A_Ergou' + name for name in ('SitDown', 'Sit', 'StandUp', 'Carry')}
MUSIC = '/Game/Sound/Music/this_town_so_wonky'
CATALOG = {
    'sofa': ('沙发', '座位很高的旧沙发，前面散着几块软垫', 'Sofa', False),
    'chair': ('高脚凳', '屋里的高脚凳', 'Chair', False),
    'phonograph': ('留声机', '放着留声机的长桌，两端空着，可以听音乐、摆放抱枕', 'Phonograph', False),
    'pillow1': ('抱枕一', '家里的软抱枕，可以搬动和用来布置', 'Pillow1', True),
    'pillow2': ('抱枕二', '家里的软抱枕，可以搬动和用来布置', 'Pillow2', True),
    'bookshelf': ('书架', '屋里原有的书架', 'Bookshelf', False),
    'bookshelf2': ('书架二', '屋里原有的第二个书架', 'Bookshelf2', False),
    'closet': ('柜子', '屋里原有的柜子', 'Closet', False),
    'vane': ('摆件一', '屋里原有的装饰摆件', 'Decoration1', False),
    'vane2': ('摆件二', '屋里原有的第二个装饰摆件', 'Decoration2', False),
    'tv2': ('电视', '屋里原有的电视', 'Television', False),
}
EXPECTED_MESH = {label: '/Game/model/indoor/' + ('vane' if label == 'vane2' else 'tv' if label == 'tv2' else label)
                 for label in CATALOG}


def object_path(value):
    return value.get_path_name() if value else None


def load(path):
    value = unreal.load_asset(path)
    if not value:
        raise RuntimeError('缺少资产：' + path)
    return value


def call_json(value):
    result = json.loads(value)
    if 'error' in result:
        raise RuntimeError(result['error'])
    return result


def package_name(path):
    return path.split('.', 1)[0]


def source_file(project, path, map_file=False):
    package = package_name(path)
    if not package.startswith('/Game/'):
        raise RuntimeError('只能处理本项目包：' + path)
    result = (project / 'Content' / (package[6:] + ('.umap' if map_file else '.uasset'))).resolve()
    if not result.is_relative_to((project / 'Content').resolve()):
        raise RuntimeError('资源路径超出项目')
    return result


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_asset(path):
    asset = load(path)
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError('资源保存失败：' + path)


def furniture_configuration_hash(item):
    keys = ('display_name','description','physical_prop','blueprint_path','definition_path','collision_mesh_path','slots',
            'audio_volume','audio_inner_radius','audio_falloff','carry_scale_multiplier','restore_scale_from_support_bottom','prop_mass_kg')
    data = {key: item[key] for key in keys if key in item}
    return hashlib.sha256(json.dumps(data, sort_keys=True, ensure_ascii=True, separators=(',',':')).encode('utf-8')).hexdigest()


def verify_installed_configuration(item, observed):
    """重跑时也核对实际用途参数，避免旧的合法资源被当成新 manifest 的结果"""
    for key in ('display_name','description','carry_scale_multiplier','restore_scale_from_support_bottom'):
        if key in item and not close_numbers(item[key], observed.get(key)):
            raise RuntimeError('家具实际配置与 manifest 不同，需审阅并显式 Replace：'+item['label']+'/'+key)
    expected_uses = sorted({slot['operation'] for slot in item['slots']})
    if sorted(observed.get('allowed_uses', [])) != expected_uses:
        raise RuntimeError('家具实际 AllowedUses 与 manifest 不同，停止保存：'+item['label'])
    if item['slots'] and (not observed.get('smart_object_reference_valid') or
        not observed.get('smart_object_component') or
        observed.get('smart_object_reference') != observed.get('smart_object_component') or
        package_name(observed.get('definition') or '') != package_name(item.get('definition_path') or '')):
        raise RuntimeError('家具实际 SmartObject 引用或 Definition 不符，停止保存：'+item['label'])
    if item['physical_prop']:
        if package_name(observed.get('mesh') or '') != package_name(item['collision_mesh_path']):
            raise RuntimeError('抱枕实例未使用独立碰撞网格，停止保存：'+item['label'])
        if (not all(observed.get(key) is True for key in ('static_mesh_is_root','grabbable_uses_root','movable',
            'simulate_physics_configured','query_and_physics','replicates','replicate_movement',
            'root_component_replicates','static_mesh_replicate_movement')) or
            observed.get('collision_profile') != 'PhysicsActor'):
            raise RuntimeError('抱枕实际根刚体、抓取或复制配置不符，停止保存：'+item['label'])
        if not observed.get('mass_override_enabled') or not close_numbers(item['prop_mass_kg'], observed.get('prop_mass_kg')):
            raise RuntimeError('Physical prop mass override does not match manifest: '+item['label'])
    if {'PlayAudio','StopAudio'} & set(expected_uses):
        audio = observed.get('audio_settings') or {}
        if (not observed.get('audio_reference_valid') or
            not all(audio.get(key) is True for key in ('override_attenuation','attenuate','spatialize','sphere')) or
            audio.get('auto_activate') is not False or audio.get('auto_destroy') is not False):
            raise RuntimeError('留声机实际声源引用或空间音频配置不符，停止保存：'+item['label'])
        for field, actual, default in (('audio_volume','volume',.45), ('audio_inner_radius','inner_radius',200),
                                       ('audio_falloff','falloff',1200)):
            if not close_numbers(item.get(field, default), audio.get(actual)):
                raise RuntimeError('留声机实际音频参数与 manifest 不同，停止保存：'+item['label']+'/'+field)
    slots = {slot['slot_id']: slot for slot in observed.get('slots', [])}
    if len(slots) != len(item['slots']):
        raise RuntimeError('家具实际槽位数量不符：'+item['label'])
    for expected in item['slots']:
        actual = slots.get(expected['slot_id'])
        if not actual or not actual['configuration_valid']:
            raise RuntimeError('家具实际槽位不可用：'+item['label'])
        if (actual.get('use_prop_transfer_point') is not expected.get('use_prop_transfer_point', False) or
            not close_numbers(expected.get('prop_transfer_offset', [0,0,0]), actual.get('prop_transfer_offset'))):
            raise RuntimeError('槽位实际道具中转点与 manifest 不同，停止保存：'+item['label']+'/'+expected['slot_id'])
        for key in ('slot_id','use_id','operation','slot','entrance','use','exit','sight_offset','approach_radius','use_radius',
                    'use_seconds','maximum_prop_half_extent','placement_tolerance','placement_angle_tolerance','maximum_support_gap'):
            if key in expected and not close_numbers(expected[key], actual.get(key)):
                raise RuntimeError('槽位实际参数与 manifest 不同，需审阅并显式 Replace：'+item['label']+'/'+expected['slot_id']+'/'+key)
        for key in ('enter_animation','animation','exit_animation','seat_mount_animation','seat_dismount_animation','sound'):
            if package_name(expected.get(key) or '') != package_name(actual.get(key) or ''):
                raise RuntimeError('槽位实际资源引用与 manifest 不同：'+item['label']+'/'+expected['slot_id']+'/'+key)


def xyz(value):
    return [value.x, value.y, value.z]


def transform(value):
    rotation = value.rotation.rotator()
    return {'location': xyz(value.translation), 'rotation': [rotation.pitch, rotation.yaw, rotation.roll],
            'scale': xyz(value.scale3d)}


def capture_world(actors):
    return {actor.get_path_name(): {'class': actor.get_class().get_path_name(),
                                  'label': actor.get_actor_label(), 'transform': transform(actor.get_actor_transform())}
            for actor in actors}


def close_numbers(left, right, tolerance=.001):
    if isinstance(left, dict):
        return isinstance(right, dict) and left.keys() == right.keys() and all(close_numbers(left[key], right[key], tolerance) for key in left)
    if isinstance(left, list):
        return isinstance(right, list) and len(left) == len(right) and all(close_numbers(a, b, tolerance) for a, b in zip(left, right))
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return math.isfinite(left) and math.isfinite(right) and abs(left - right) <= tolerance
    return left == right


def pending_packages():
    util = unreal.EditorLoadingAndSavingUtils
    return [object_path(package) for package in list(util.get_dirty_map_packages()) + list(util.get_dirty_content_packages())]


def inspect_scene():
    tools = unreal.DeliveryHomeObjectAssetTools
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not world or package_name(world.get_path_name()) != MAP:
        raise RuntimeError('必须已打开正式地图；脚本不会切换地图')
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    homes = [actor for actor in actors if isinstance(actor, unreal.DeliveryHome)]
    if len(homes) != 1:
        raise RuntimeError('共享 Home 必须唯一')
    house = homes[0].get_editor_property('existing_house')
    if not house or house.get_actor_label() != 'house2':
        raise RuntimeError('Home.ExistingHouse 与 house2 不一致')
    # 第一次 native 查询即校验 Editor World、无 PIE、HasBegunPlay=false
    house_snapshot = call_json(tools.inspect_furniture(house))
    entries = {}
    for label in CATALOG:
        matches = [actor for actor in actors if actor.get_actor_label() == label]
        if len(matches) != 1:
            raise RuntimeError('家具标签缺失或重复：' + label)
        actor = matches[0]
        data = call_json(tools.inspect_furniture(actor))
        original = EXPECTED_MESH[label]
        derived = ROOT + '/Meshes/SM_Home' + CATALOG[label][2]
        if package_name(data['mesh']) not in (original, derived):
            raise RuntimeError('家具已更换未知网格：' + label)
        entries[label] = {'actor': actor, 'data': data}
    return homes[0], actors, entries, house_snapshot


def slot_template(name, operation, display):
    # 未标定留空，不能悄悄把网格中心当成可用座位或入口
    result = {'slot_id': name, 'use_id': name, 'operation': operation, 'display_name': display,
              'slot': None, 'entrance': None, 'use': None, 'exit': None,
              'sight_offset': [0, 0, 40], 'approach_radius': 1200, 'use_radius': 180, 'use_seconds': 1}
    if operation == 'Sit':
        result.update(enter_animation=ANIMATIONS['SitDown'], animation=ANIMATIONS['Sit'], exit_animation=ANIMATIONS['StandUp'])
    if operation == 'PlayAudio':
        result['sound'] = MUSIC
    if operation == 'PlaceProp':
        result.update(maximum_prop_half_extent=None, placement_tolerance=15, placement_angle_tolerance=15, maximum_support_gap=12,
                      use_prop_transfer_point=False, prop_transfer_offset=[0,0,0])
    return result


def animation_baseline():
    cls = unreal.EditorAssetLibrary.load_blueprint_class(NPC)
    if not cls:
        raise RuntimeError('缺少二狗蓝图：' + NPC)
    npc = unreal.get_default_object(cls)
    mesh_component = npc.get_editor_property('mesh')
    mesh = mesh_component.get_editor_property('skeletal_mesh_asset')
    idle = npc.get_editor_property('presentation').get_editor_property('idle_animation')
    if not mesh or not idle or idle.get_editor_property('skeleton') != mesh.get_editor_property('skeleton'):
        raise RuntimeError('二狗实际模型与 Idle 骨架不匹配')
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property('evaluation_type', unreal.AnimDataEvalType.COMPRESSED)
    options.set_editor_property('optional_skeletal_mesh', mesh)
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(idle, 0, options)
    available = {str(name) for name in unreal.AnimPoseExtensions.get_bone_names(pose)}
    bones = ['Hips', 'Spine', 'Spine1', 'Spine2', 'Head', 'LeftUpLeg', 'LeftLeg', 'LeftFoot',
             'RightUpLeg', 'RightLeg', 'RightFoot', 'LeftArm', 'LeftForeArm', 'LeftHand', 'RightArm', 'RightForeArm', 'RightHand']
    bones = [name for name in bones if name in available]
    return {'mesh': object_path(mesh), 'idle': object_path(idle), 'skeleton': object_path(mesh.get_editor_property('skeleton')),
            'mesh_relative_transform': transform(mesh_component.get_relative_transform()),
            'idle_bones_mesh_cm': {name: transform(unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)) for name in bones}}, mesh


def unique_companion(actors, expected_path=None):
    companions = [actor for actor in actors if isinstance(actor, unreal.DeliveryCompanionCharacter)]
    if len(companions) != 1:
        raise RuntimeError('二狗实例必须唯一')
    actor = companions[0]
    if actor.get_class() != unreal.EditorAssetLibrary.load_blueprint_class(NPC) or (expected_path and actor.get_path_name() != expected_path):
        raise RuntimeError('二狗实例身份或蓝图类型已变化')
    return actor


def companion_snapshot(actor):
    body = actor.get_editor_property('mesh')
    mesh = body.get_editor_property('skeletal_mesh_asset')
    companion = actor.get_editor_property('companion')
    profile = actor.get_editor_property('profile')
    presentation = actor.get_editor_property('presentation')
    mesh_components = {}
    for component in actor.get_components_by_class(unreal.MeshComponent):
        asset = (component.get_editor_property('skeletal_mesh_asset') if isinstance(component, unreal.SkeletalMeshComponent)
                 else component.get_editor_property('static_mesh') if isinstance(component, unreal.StaticMeshComponent) else None)
        mesh_components[component.get_name()] = {'class': component.get_class().get_path_name(), 'mesh': object_path(asset),
            'transform': transform(component.get_relative_transform()),
            'overrides': [object_path(value) for value in component.get_editor_property('override_materials')],
            'materials': [object_path(component.get_material(index)) for index in range(component.get_num_materials())]}
    capsule = actor.get_editor_property('capsule_component')
    return {'path': object_path(actor),
            'binding': {'anim_class': object_path(body.get_editor_property('anim_class')),
                'animation_mode': str(body.get_editor_property('animation_mode')),
                'carry_animation': object_path(companion.get_editor_property('prop_carry_animation')),
                'socket': str(companion.get_editor_property('prop_carry_socket')),
                'offset': xyz(companion.get_editor_property('prop_carry_offset'))},
            'preserved': {'actor_transform': transform(actor.get_actor_transform()),
                'mesh_components': mesh_components,
                'capsule_radius_cm': capsule.get_unscaled_capsule_radius(), 'capsule_half_height_cm': capsule.get_unscaled_capsule_half_height(),
                'mesh': object_path(mesh), 'skeleton': object_path(mesh.get_editor_property('skeleton')) if mesh else None,
                'mesh_transform': transform(body.get_relative_transform()),
                'overrides': [object_path(value) for value in body.get_editor_property('override_materials')],
                'materials': [object_path(body.get_material(index)) for index in range(body.get_num_materials())],
                'post_process': object_path(mesh.get_editor_property('post_process_anim_blueprint')) if mesh else None,
                'post_process_disabled': bool(body.get_editor_property('disable_post_process_blueprint')),
                'display_name': str(actor.get_editor_property('display_name')),
                'persona': actor.get_editor_property('agent').get_editor_property('persona'),
                'profile_table': object_path(profile.get_editor_property('data_table')),
                'profile_row': str(profile.get_editor_property('row_name')),
                'behavior_tree': object_path(actor.get_editor_property('behavior_tree')),
                'companion_tuning': object_path(companion.get_editor_property('tuning')),
                'voice_set': object_path(presentation.get_editor_property('voice_set')),
                'tts_voice': str(presentation.get_editor_property('tts_voice')),
                'voice_frequency': float(presentation.get_editor_property('voice_frequency')),
                'body_animations': {name: object_path(presentation.get_editor_property(name+'_animation'))
                    for name in ('idle','talk','walk','pace','hurry','hold','attack','down','get_up','hit')}}}


def inspect_companion_binding(actors):
    bp = load(NPC)
    if not isinstance(bp, unreal.Blueprint) or not bp.generated_class():
        raise RuntimeError('二狗蓝图没有有效生成类')
    return {'defaults': companion_snapshot(unreal.get_default_object(bp.generated_class())),
            'instance': companion_snapshot(unique_companion(actors))}


def require_home_animation_ready(actors, baseline, animations, carry_config):
    """校验可迁移的旧接线与独立 wrapper，地图尚未绑定时也能进入正式 Setup"""
    body_bp = load(carry_config['anim_blueprint'])
    if not isinstance(body_bp, unreal.AnimBlueprint) or not body_bp.generated_class():
        raise RuntimeError('独立家居动画蓝图没有有效生成类')
    if (unreal.EditorAssetLibrary.get_metadata_tag(body_bp, 'DeliveryCompanionHomeBodyGraph') != '2' or
        object_path(unreal.BlueprintEditorLibrary.get_blueprint_parent_class(body_bp)) != '/Script/Delivery.DeliveryCompanionBodyAnimInstance' or
        object_path(body_bp.get_editor_property('target_skeleton')) != baseline['skeleton']):
        raise RuntimeError('独立 wrapper 的归属、版本、父类或骨架不符')
    source_path = unreal.EditorAssetLibrary.get_metadata_tag(body_bp, 'DeliveryCompanionHomeBodySource')
    source = load(source_path)
    if not isinstance(source, unreal.AnimBlueprint) or object_path(source.get_editor_property('target_skeleton')) != baseline['skeleton']:
        raise RuntimeError('wrapper 保存的原身体动画来源无效')
    if package_name(unreal.EditorAssetLibrary.get_metadata_tag(body_bp, 'DeliveryCompanionHomeCarry')) != package_name(animations['Carry']):
        raise RuntimeError('wrapper 保存的搬运动作与 manifest 不一致')
    graph = call_json(unreal.DeliveryCompanionAnimationAssetTools.inspect_companion_body_animation())
    # 本机原生 Inspect 的 success 同时要求 CDO 已绑定；该 reason 只在 GraphMatches=true 时返回
    if not graph.get('success') and graph.get('reason') != 'Independent home wrapper inspected; map instances were not checked':
        raise RuntimeError('现有家居 wrapper 结构检查失败：'+str(graph))
    current = inspect_companion_binding(actors)
    allowed_classes = {object_path(source.generated_class()), object_path(body_bp.generated_class())}
    desired_carry = object_path(load(animations['Carry']))
    for label, entry in current.items():
        if entry['preserved']['mesh'] != baseline['mesh'] or entry['preserved']['skeleton'] != baseline['skeleton']:
            raise RuntimeError('二狗模型或骨架与蓝图动画基准不同：'+label)
        binding = entry['binding']
        if binding['anim_class'] not in allowed_classes or binding['animation_mode'] != str(unreal.AnimationMode.ANIMATION_BLUEPRINT):
            raise RuntimeError('保留未知人物动画覆盖：'+label)
        if binding['carry_animation'] not in (None, desired_carry):
            raise RuntimeError('保留未知搬运动画覆盖：'+label)
        old_anchor = binding['socket'] in ('None','') and close_numbers(binding['offset'], [70,0,0])
        selected_anchor = binding['socket'] == carry_config['socket'] and close_numbers(binding['offset'], carry_config['offset'])
        if not old_anchor and not selected_anchor:
            raise RuntimeError('保留未知搬运挂点覆盖：'+label)
    for path in animations.values():
        animation = load(path)
        if unreal.EditorAssetLibrary.get_metadata_tag(animation, 'DeliveryHomeAnimationRetarget') != '1' or unreal.EditorAssetLibrary.get_metadata_tag(animation, 'SourceBasisAlignment') != 'source_y_forward_to_x_forward_v1':
            raise RuntimeError('家居动作不是已对齐的本批原生成品重定向：'+path)
        source_animation = load(unreal.EditorAssetLibrary.get_metadata_tag(animation, 'SourceAnimation'))
        if (not package_name(object_path(source_animation)).startswith(ROOT+'/Source/QuaterniusAligned/') or
            unreal.EditorAssetLibrary.get_metadata_tag(source_animation, 'DeliveryHomeAnimationSource') != '1' or
            unreal.EditorAssetLibrary.get_metadata_tag(source_animation, 'SourceBasisAlignment') != 'source_y_forward_to_x_forward_v1' or
            not re.fullmatch(r'[0-9a-fA-F]{64}', unreal.EditorAssetLibrary.get_metadata_tag(animation, 'SourceSHA256')) or
            unreal.EditorAssetLibrary.get_metadata_tag(source_animation, 'SourceSHA256') != unreal.EditorAssetLibrary.get_metadata_tag(animation, 'SourceSHA256')):
            raise RuntimeError('家居动作保存的作者源身份不符：'+path)
    for path in animations.values():
        animation = load(path)
        if not isinstance(animation, unreal.AnimSequence) or object_path(animation.get_editor_property('skeleton')) != baseline['skeleton'] or animation.get_editor_property('enable_root_motion'):
            raise RuntimeError('家居动画骨架或 Root Motion 配置不符：' + path)
        if not math.isfinite(animation.get_play_length()) or animation.get_play_length() <= 0:
            raise RuntimeError('成品家居动画时长无效：' + path)
    actor = unique_companion(actors)
    if not actor.get_editor_property('mesh').does_socket_exist(carry_config['socket']):
        raise RuntimeError('实际二狗骨架不存在明确配置的搬运挂点')
    return {'before': current, 'wrapper': graph, 'source_blueprint': source_path,
            'desired_class': object_path(body_bp.generated_class()), 'desired_carry': desired_carry}


def apply_companion_binding(plan, carry_config, animations):
    """只修改身体动画接线与搬运挂点，编译重实例化后重新取得 CDO 和地图对象"""
    bp = load(NPC)
    body_class = unreal.EditorAssetLibrary.load_blueprint_class(carry_config['anim_blueprint'])
    animation = load(animations['Carry'])
    def apply(actor):
        body, companion = actor.get_editor_property('mesh'), actor.get_editor_property('companion')
        actor.modify(); body.modify(); companion.modify()
        body.set_editor_property('animation_mode', unreal.AnimationMode.ANIMATION_BLUEPRINT)
        body.set_editor_property('anim_class', body_class)
        companion.set_editor_property('prop_carry_animation', animation)
        companion.set_editor_property('prop_carry_socket', unreal.Name(carry_config['socket']))
        companion.set_editor_property('prop_carry_offset', unreal.Vector(*carry_config['offset']))
    bp.modify()
    apply(unreal.get_default_object(bp.generated_class()))
    # UE5.8 原生入口返回 bool，内部 SkipSave；保存留到全部地图检查通过之后
    if not unreal.BlueprintEditorLibrary.compile_blueprint(bp):
        raise RuntimeError('二狗接线后蓝图编译失败，地图保持未保存')
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    actor = unique_companion(actors, plan['before']['instance']['path'])
    apply(actor)
    return verify_companion_binding(plan, carry_config, animations)


def verify_companion_binding(plan, carry_config, animations):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    current = inspect_companion_binding(actors)
    desired = {'anim_class': plan['desired_class'], 'animation_mode': str(unreal.AnimationMode.ANIMATION_BLUEPRINT),
               'carry_animation': object_path(load(animations['Carry'])), 'socket': carry_config['socket'], 'offset': carry_config['offset']}
    for label, entry in current.items():
        if not close_numbers(entry['binding'], desired):
            raise RuntimeError('二狗实际接线读回与 manifest 不同：'+label)
        if not close_numbers(entry['preserved'], plan['before'][label]['preserved']):
            raise RuntimeError('二狗模型、材质、变换、声音或既有行为发生变化：'+label)
    graph = call_json(unreal.DeliveryCompanionAnimationAssetTools.inspect_companion_body_animation())
    if not graph.get('success') or not graph.get('bound_companion_defaults'):
        raise RuntimeError('原生 wrapper 检查或蓝图默认接线读回失败')
    return {'readback': current, 'defaults_match_instance': close_numbers(current['defaults']['binding'], current['instance']['binding']),
            'wrapper': graph}


def manifest_draft(home, entries, baseline):
    objects = []
    for label, (display, description, suffix, physical) in CATALOG.items():
        observed = entries[label]['data']
        object_id = observed.get('object_id')
        if not object_id or uuid.UUID(object_id).int == 0:
            object_id = str(uuid.uuid5(uuid.NAMESPACE_URL, 'Delivery:' + MAP + ':' + observed['actor']))
        slots = []
        if label in ('sofa', 'chair'):
            slots.append(slot_template('SeatMain', 'Sit', '坐下'))
        if label == 'sofa':
            slots.extend([slot_template('PillowLeft', 'PlaceProp', '把抱枕放在沙发左侧'),
                          slot_template('PillowRight', 'PlaceProp', '把抱枕放在沙发右侧')])
        if label == 'phonograph':
            slots.extend([slot_template('Play', 'PlayAudio', '播放音乐'), slot_template('Stop', 'StopAudio', '停止音乐')])
        if physical:
            slots.append(slot_template('Take', 'TakeProp', '拿起抱枕'))
        item = {'label': label, 'object_id': object_id, 'expected_actor': observed['actor'],
                'expected_mesh': observed['mesh'], 'expected_transform': observed['transform'], 'expected_overrides': observed['overrides'],
                'blueprint_path': ROOT + '/Blueprints/BP_Home' + suffix,
                'display_name': display, 'description': description, 'physical_prop': physical, 'slots': slots}
        if slots:
            item['definition_path'] = ROOT + '/SmartObjects/SO_Home' + suffix
        if physical:
            item['collision_mesh_path'] = ROOT + '/Meshes/SM_Home' + suffix
            item['prop_mass_kg'] = 2.0
        if label == 'phonograph':
            # 沿用已认可的较轻混音，独立声源仍待实际听感核对
            item.update(audio_volume=.22, audio_inner_radius=200, audio_falloff=1200)
        objects.append(item)
    evidence = Path(unreal.Paths.project_dir()).resolve()/CARRY_EVIDENCE
    return {'schema_version': 1, 'map': MAP, 'home': home.get_path_name(), 'calibrated': False,
            'interior_anchor_world': xyz(home.get_editor_property('interior_anchor').get_world_location()),
            'companion_carry': {'calibrated': False, 'anim_blueprint': BODY_BLUEPRINT, 'socket': 'Spine2',
                'offset': [4.54, -5.01, 35.53], 'derived_from_evidence': CARRY_EVIDENCE,
                'evidence_sha256': sha256(evidence) if evidence.is_file() else None,
                'notes': '真实上半身采样的后侧双角持物候选；忽略骨骼导入缩放，尚待衣服与手指接触画面核对'},
            'objects': objects, 'animations': {'baseline': baseline, 'assets': dict(ANIMATIONS)}}


def backup_packages(project, output, packages):
    result = {}
    for path in sorted(set(packages)):
        filename = source_file(project, path, package_name(path) == MAP)
        for suffix in (filename.suffix, '.uexp', '.ubulk'):
            source = filename.with_suffix(suffix)
            if not source.is_file():
                continue
            target = output / 'Backup' / source.relative_to(project)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            result[str(source)] = {'sha256': sha256(source), 'backup': str(target)}
    # 标准替换保留外部 Actor 包名；若地图采用外部包，同时备份其真实文件
    for directory in (project/'Content/__ExternalActors__/Level/TestForCharacter', project/'Content/Level/__ExternalActors__/TestForCharacter'):
        if directory.is_dir():
            for source in directory.rglob('*'):
                if source.is_file():
                    target = output/'Backup'/source.relative_to(project)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)
                    result[str(source)] = {'sha256': sha256(source), 'backup': str(target)}
    return result


def protected_files(project, entries, baseline, animations, setup=False):
    paths = {EXPECTED_MESH[label] for label in CATALOG}
    paths.update((baseline['mesh'], baseline['skeleton'], baseline['idle'], '/Game/model/buildings/house2', MUSIC, NPC))
    if unreal.EditorAssetLibrary.does_asset_exist(BODY_BLUEPRINT):
        paths.add(BODY_BLUEPRINT)
        source = unreal.EditorAssetLibrary.get_metadata_tag(load(BODY_BLUEPRINT), 'DeliveryCompanionHomeBodySource')
        if source and source.startswith('/Game/'):
            paths.add(source)
    for entry in entries.values():
        paths.update(path for path in entry['data']['materials'] if path and path.startswith('/Game/'))
    paths.update(path for path in animations.values() if source_file(project, path).is_file())
    result = {str(source_file(project, path)): sha256(source_file(project, path)) for path in paths}
    for directory in ('NPC/Residents', 'NPC/Grandpa', 'UI'):
        for path in (project/'Content'/directory).rglob('*.uasset'):
            result[str(path)] = sha256(path)
    # Setup 只允许修改明确列出的角色蓝图，模型、原动画和独立 wrapper 继续受哈希保护
    if setup:
        result.pop(str(source_file(project, NPC)), None)
    return result


def validate_manifest(manifest, home, entries, baseline, setup):
    if manifest.get('schema_version') != 1 or manifest.get('map') != MAP or manifest.get('home') != home.get_path_name():
        raise RuntimeError('manifest 版本、地图或共享房屋不符')
    if setup and manifest.get('calibrated') is not True:
        raise RuntimeError('先检查座位/进出/音频/取放位置，再明确保存 calibrated=true')
    carry = manifest.get('companion_carry')
    if not isinstance(carry, dict) or package_name(carry.get('anim_blueprint', '')) != BODY_BLUEPRINT:
        raise RuntimeError('companion_carry 必须明确引用已创建的独立 ABP_CompanionBody')
    if setup and carry.get('calibrated') is not True:
        raise RuntimeError('搬运挂点仍是未校准草案，请明确审阅 companion_carry 后再保存')
    offset = carry.get('offset')
    if (not isinstance(carry.get('socket'), str) or not carry['socket'] or carry['socket'] == 'None' or
        not isinstance(offset, list) or len(offset) != 3 or
        any(isinstance(value, bool) or not isinstance(value, (int,float)) or not math.isfinite(value) for value in offset)):
        raise RuntimeError('搬运挂点必须是实际骨骼名与三个有限厘米偏移')
    project = Path(unreal.Paths.project_dir()).resolve()
    evidence = (project/carry.get('derived_from_evidence', '')).resolve()
    if not evidence.is_relative_to((project/'Saved').resolve()) or not evidence.is_file() or sha256(evidence) != carry.get('evidence_sha256'):
        raise RuntimeError('搬运挂点的标定依据缺失或 SHA 已变化')
    items = manifest.get('objects', [])
    if len(items) != len(CATALOG) or {item['label'] for item in items} != set(CATALOG):
        raise RuntimeError('manifest 家具清单不完整或重复')
    ids = [str(uuid.UUID(item['object_id'])) for item in items]
    if len(set(ids)) != len(ids) or any(uuid.UUID(value).int == 0 for value in ids):
        raise RuntimeError('ObjectId 必须是唯一非零 UUID')
    for item in items:
        if item['physical_prop']:
            mass = item.get('prop_mass_kg')
            if isinstance(mass, bool) or not isinstance(mass, (int,float)) or not math.isfinite(mass) or mass <= 0:
                raise RuntimeError('physical_prop requires explicit finite positive prop_mass_kg: '+item['label'])
        expected_prefix = ROOT + '/'
        for key in ('blueprint_path', 'definition_path', 'collision_mesh_path'):
            if key in item and (not item[key].startswith(expected_prefix) or '..' in item[key] or '.' in item[key]):
                raise RuntimeError('写入路径必须位于同伴独立家居目录：' + item[key])
        data = entries[item['label']]['data']
        if data['actor'] != item['expected_actor'] or not close_numbers(data['transform'], item['expected_transform']):
            raise RuntimeError('家具路径或变换已变化：' + item['label'])
        if data['overrides'] != item['expected_overrides']:
            raise RuntimeError('家具材质覆盖已变化：' + item['label'])
        allowed_meshes = {package_name(item['expected_mesh']), item.get('collision_mesh_path', '')}
        if package_name(data['mesh']) not in allowed_meshes:
            raise RuntimeError('家具网格已变化：' + item['label'])
        if data.get('object_id') and str(uuid.UUID(data['object_id'])) != str(uuid.UUID(item['object_id'])):
            raise RuntimeError('不能重新分配已保存家具的 ObjectId')
    recorded = manifest['animations']['baseline']
    imported = manifest['animations'].get('assets', {})
    if set(imported) != set(ANIMATIONS) or any(not isinstance(path, str) or not path.startswith('/Game/') for path in imported.values()):
        raise RuntimeError('animations.assets 必须明确指定已有 SitDown/Sit/StandUp/Carry 成品资产')
    for item in items:
        for slot in item['slots']:
            transfer_enabled = slot.get('use_prop_transfer_point', False)
            transfer_offset = slot.get('prop_transfer_offset', [0,0,0])
            if type(transfer_enabled) is not bool or (transfer_enabled and slot['operation'] != 'PlaceProp'):
                raise RuntimeError('use_prop_transfer_point 必须是 bool 且仅 PlaceProp 可启用：'+item['label'])
            if (not isinstance(transfer_offset, list) or len(transfer_offset) != 3 or
                any(isinstance(value, bool) or not isinstance(value, (int,float)) or not math.isfinite(value) for value in transfer_offset)):
                raise RuntimeError('prop_transfer_offset 必须是三个有限厘米数值：'+item['label'])
            if slot['operation'] == 'Sit' and any(package_name(slot.get(field, '')) != package_name(imported[role]) for field, role in
                                                 (('enter_animation', 'SitDown'), ('animation', 'Sit'), ('exit_animation', 'StandUp'))):
                raise RuntimeError('座位引用与成品动画清单不一致')
            if slot['operation'] == 'Sit':
                for key in ('seat_mount_animation', 'seat_dismount_animation'):
                    path = slot.get(key, '')
                    if path and (not isinstance(path, str) or not path.startswith('/Game/NPC/Companion/Home/') or '..' in path):
                        raise RuntimeError('上下座位动作必须来自明确导入的独立成品资源：'+item['label']+'/'+key)
                    if path:
                        clip = load(path)
                        if (not isinstance(clip, unreal.AnimSequence) or
                            object_path(clip.get_editor_property('skeleton')) != baseline['skeleton'] or
                            unreal.EditorAssetLibrary.get_metadata_tag(clip, 'DeliverySeatAnimation') != '1' or
                            unreal.EditorAssetLibrary.get_metadata_tag(clip, 'SourceBasisAlignment') != 'source_y_forward_to_x_forward_v1' or
                            clip.get_editor_property('enable_root_motion') or not clip.get_editor_property('force_root_lock') or
                            clip.get_editor_property('root_motion_root_lock') != unreal.RootMotionRootLock.ANIM_FIRST_FRAME):
                            raise RuntimeError('上下座位成品骨架、来源或根锁定不符：'+path)
    for field in ('mesh', 'skeleton', 'idle', 'mesh_relative_transform'):
        if not close_numbers(recorded[field], baseline[field]):
            raise RuntimeError('人物动画基准已变化：' + field)


def inspect_imported_animations(mesh, animations):
    """读取压缩剪辑关键帧，给后续静态预览和姿势核对保留真实坐标"""
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property('evaluation_type', unreal.AnimDataEvalType.COMPRESSED)
    options.set_editor_property('optional_skeletal_mesh', mesh)
    result = {}
    bones = ['Hips', 'Spine', 'Head', 'LeftLeg', 'LeftFoot', 'RightLeg', 'RightFoot',
             'LeftForeArm', 'LeftHand', 'RightForeArm', 'RightHand']
    for name, path in animations.items():
        asset = unreal.load_asset(path)
        if not asset:
            result[name] = {'path': path, 'exists': False}
            continue
        if not isinstance(asset, unreal.AnimSequence):
            raise RuntimeError('成品动作必须是 AnimSequence：' + path)
        duration = asset.get_play_length()
        if not math.isfinite(duration) or duration <= 0:
            raise RuntimeError('成品动画时长无效：' + path)
        skeleton_matches = asset.get_editor_property('skeleton') == mesh.get_editor_property('skeleton')
        samples = []
        for fraction in ((0, .25, .5, .75, 1) if skeleton_matches else ()):
            pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(asset, duration*fraction, options)
            available = {str(bone) for bone in unreal.AnimPoseExtensions.get_bone_names(pose)}
            samples.append({'time': duration*fraction, 'bones_mesh_cm': {
                bone: transform(unreal.AnimPoseExtensions.get_bone_pose(pose, bone, unreal.AnimPoseSpaces.WORLD)) for bone in bones if bone in available}})
        result[name] = {'path': path, 'exists': True, 'length': duration,
                        'skeleton': object_path(asset.get_editor_property('skeleton')), 'skeleton_matches': skeleton_matches,
                        'root_motion': bool(asset.get_editor_property('enable_root_motion')), 'samples': samples}
    return result


def main(setup=False, replace=False, manifest_file=None):
    project = Path(unreal.Paths.project_dir()).resolve()
    output = project/'Saved/AIResearch/HomeObjects'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'preparing', 'setup': setup, 'replace': replace,
              'map_saved': False, 'assets_saved': [], 'not_run': ['PIE', 'BeginPlay', 'game RPC', 'navigation build', 'LLM', 'TTS']}

    def write_report():
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    try:
        home, actors, entries, house = inspect_scene()
        baseline, mesh = animation_baseline()
        manifest = json.loads(Path(manifest_file).read_text(encoding='utf-8-sig')) if manifest_file else None
        animations = manifest['animations']['assets'] if manifest else ANIMATIONS
        if not unreal.DeliveryHomeObjectAssetTools.finish_pending_editor_assets():
            raise RuntimeError('场景资源加载或编译尚未完成，停止室内检查')
        interior = call_json(unreal.DeliveryHomeObjectAssetTools.inspect_interior_anchor(
            home, home.get_editor_property('interior_anchor').get_world_location()))
        report.update(house=house, furniture={label: item['data'] for label, item in entries.items()}, animation_baseline=baseline,
                      interior_anchor=interior, dirty_before=pending_packages(), world_before=capture_world(actors))
        report['companion_before'] = inspect_companion_binding(actors)
        report['imported_animations'] = inspect_imported_animations(mesh, animations)
        protected = protected_files(project, entries, baseline, animations, setup)
        if manifest:
            for item in manifest.get('objects', []):
                for slot in item.get('slots', []):
                    for key in ('seat_mount_animation', 'seat_dismount_animation'):
                        if slot.get(key):
                            file = source_file(project, slot[key])
                            if not file.is_file():
                                raise RuntimeError('上下座位成品尚未导入：'+slot[key])
                            protected[str(file)] = sha256(file)
        report['protected_before'] = protected
        draft = manifest_draft(home, entries, baseline)
        (output/'manifest_draft.json').write_text(json.dumps(draft, ensure_ascii=False, indent=2), encoding='utf-8')
        if not setup:
            report['status'] = 'inspected_read_only'
            return
        if report['dirty_before']:
            raise RuntimeError('存在预先脏包，停止写入以保护未保存改动')
        if not manifest_file:
            raise RuntimeError('写操作必须显式指定已标定的 manifest')
        validate_manifest(manifest, home, entries, baseline, setup)
        # 在任何写入之前确认 Home 已有本批正式引用字段
        if setup:
            original_furnishings = list(home.get_editor_property('furnishings'))
            proposed = unreal.Vector(*manifest['interior_anchor_world'])
            # 动画和蓝图预检会启动新的资源编译，等本轮加载完成再查询真实碰撞
            if not unreal.DeliveryHomeObjectAssetTools.finish_pending_editor_assets():
                raise RuntimeError('本轮资源加载或编译仍未完成，停止室内检查与写入')
            check = call_json(unreal.DeliveryHomeObjectAssetTools.inspect_interior_anchor(home, proposed))
            if not check['valid']:
                raise RuntimeError('室内落脚点的地面、胶囊或完整门洞路径检查失败')
            report['interior_anchor_selected'] = check
        targets = [MAP, NPC]
        for item in manifest['objects']:
            targets.extend(item[key] for key in ('blueprint_path', 'definition_path', 'collision_mesh_path') if key in item)
        report['backups'] = backup_packages(project, output, targets)
        report['manifest'] = manifest
        write_report()
        if setup:
            tools = unreal.DeliveryHomeObjectAssetTools
            binding_plan = require_home_animation_ready(actors, baseline, animations, manifest['companion_carry'])
            report['companion_binding_plan'] = binding_plan
            # 先让全部资产编译成功，任何一个失败均不替换地图中的家具
            for item in manifest['objects']:
                bp = unreal.load_asset(item['blueprint_path'])
                if bp and not replace:
                    if unreal.EditorAssetLibrary.get_metadata_tag(bp, 'DeliveryHomeAssetTool') != '1':
                        raise RuntimeError('目标蓝图存在未知资源：' + item['blueprint_path'])
                    if not bp.generated_class() or bp.get_editor_property('status') == unreal.BlueprintStatus.BS_ERROR:
                        raise RuntimeError('已有家具蓝图没有有效编译结果：'+item['blueprint_path'])
                    if unreal.EditorAssetLibrary.get_metadata_tag(bp, 'DeliveryHomeManifestConfiguration') != furniture_configuration_hash(item):
                        raise RuntimeError('已有家具未记录当前完整配置，需显式 -ReplaceCompanionHomeAssets：'+item['blueprint_path'])
                    continue
                result = call_json(tools.build_furniture_assets(entries[item['label']]['actor'], json.dumps(item, ensure_ascii=False), replace))
                unreal.EditorAssetLibrary.set_metadata_tag(load(item['blueprint_path']), 'DeliveryHomeManifestConfiguration', furniture_configuration_hash(item))
                for path in result['assets_to_save']:
                    save_asset(path)
                    report['assets_saved'].append(path)
            report['status'] = 'assets_ready_map_not_saved'
            write_report()
            # 家具资源全部就绪后才修改人物接线，默认与实例分别写入并立即读回
            report['companion_binding'] = apply_companion_binding(binding_plan, manifest['companion_carry'], animations)
            write_report()
            installed = []
            for item in manifest['objects']:
                bp = load(item['blueprint_path'])
                identity, valid = unreal.GuidLibrary.parse_string_to_guid(item['object_id'])
                if not valid:
                    raise RuntimeError('UE GUID 解析失败')
                actor = tools.install_furniture(entries[item['label']]['actor'], bp, home, identity)
                if not actor:
                    raise RuntimeError('家具替换失败，地图保持未保存：' + item['label'])
                observed = call_json(tools.inspect_furniture(actor))
                if not close_numbers(observed['transform'], item['expected_transform']) or observed['overrides'] != item['expected_overrides']:
                    raise RuntimeError('替换后变换或材质覆盖不一致，地图保持未保存：' + item['label'])
                if observed['materials'] != entries[item['label']]['data']['materials'] or not observed['replicates']:
                    raise RuntimeError('家具实际材质或复制配置不符')
                if item['physical_prop'] and not (observed['movable'] and observed['simulate_physics_configured'] and observed['replicate_movement']):
                    raise RuntimeError('枕头缺少真实根刚体或位姿复制')
                expected_slots = {(slot['slot_id'], slot['use_id'], slot['operation']) for slot in item['slots']}
                actual_slots = {(slot['slot_id'], slot['use_id'], slot['operation']) for slot in observed.get('slots', [])}
                if actual_slots != expected_slots or any(not slot['configuration_valid'] for slot in observed.get('slots', [])):
                    raise RuntimeError('实际 Smart Object 用途配置与 manifest 不一致')
                verify_installed_configuration(item, observed)
                if str(uuid.UUID(observed['object_id'])) != str(uuid.UUID(item['object_id'])) or observed['home'] != home.get_path_name():
                    raise RuntimeError('家具身份或 Home 绑定读回失败')
                installed.append(actor)
                report.setdefault('installed', {})[item['label']] = observed
            remaining = [actor for actor in original_furnishings if actor and actor.get_path_name() not in {item['expected_actor'] for item in manifest['objects']}]
            home.set_editor_property('furnishings', remaining + installed)
            home.get_editor_property('interior_anchor').set_world_location(proposed, False, False)
            after = capture_world(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors())
            targeted = {item['expected_actor'] for item in manifest['objects']}
            before_other = {path: value for path, value in report['world_before'].items() if path not in targeted}
            after_other = {path: value for path, value in after.items() if path not in targeted}
            if not close_numbers(before_other, after_other):
                raise RuntimeError('非目标 Actor 的身份或变换变化，停止保存地图')
            report['world_after'] = after
            report['navigation_actor_count'] = sum('NavMeshBoundsVolume' in value['class'] for value in after.values())
            if any(sha256(Path(path)) != value for path, value in protected.items()):
                raise RuntimeError('受保护源资产哈希变化，停止保存地图')
            report['companion_binding'] = verify_companion_binding(binding_plan, manifest['companion_carry'], animations)
            # 蓝图和地图不是跨包事务，分别记下保存结果；失败可用同轮备份恢复
            save_asset(NPC)
            report['assets_saved'].append(NPC)
            report['companion_blueprint_saved'] = True
            report['companion_binding_after_blueprint_save'] = verify_companion_binding(binding_plan, manifest['companion_carry'], animations)
            write_report()
            if any(sha256(Path(path)) != value for path, value in protected.items()):
                raise RuntimeError('角色蓝图保存影响受保护资源，停止保存地图')
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
            if not world or not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
                raise RuntimeError('地图保存失败')
            report['map_saved'] = True
            report['companion_binding_after_map_save'] = verify_companion_binding(binding_plan, manifest['companion_carry'], animations)
        report['protected_after'] = {path: sha256(Path(path)) for path in protected}
        if report['protected_after'] != protected:
            raise RuntimeError('原家具/人物/居民/爷爷/UI 资产哈希变化')
        report['status'] = 'saved_needs_independent_reload_and_runtime_acceptance'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = repr(error)
        unreal.log_error(str(error))
        raise
    finally:
        write_report()
        unreal.log('Home objects report: ' + str(output/'report.json'))


if __name__ == '__main__':
    command_line = unreal.SystemLibrary.get_command_line()
    switches = command_line.lower()
    setup = '-setupcompanionhomeobjects' in switches
    inspect = '-inspectcompanionhomeobjects' in switches
    if inspect and setup:
        raise RuntimeError('Inspect 不能与写操作并用')
    match = re.search(r'(?:^|\s)-HomeObjectsManifest=(?:"([^"]+)"|(\S+))', command_line, re.IGNORECASE)
    main(setup, '-replacecompanionhomeassets' in switches,
         (match.group(1) or match.group(2)) if match else None)
