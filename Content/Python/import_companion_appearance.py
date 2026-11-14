"""绑定项目已有小男孩与卡通老人，保留声音、动作和轻量 UI，不保存地图

默认只读；-BindExistingCompanionAppearance 绑定小男孩并清理旧老板 SCS/演出图
-BindExistingNoodleBoss 复制已有卡通老人及同骨架动作，再绑定独立场景
背包使用独立网格挂点抵消骨骼导入缩放，不改变共享骨架
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import unreal

COMPANION_BP = '/Game/NPC/Companion/BP_Companion_Ergou'
SCENE_BP = '/Game/NPC/Companion/Encounter/BP_NoodleEncounter'
BOY_SOURCE = '/Game/Characters/NPC/SK_NpcBoy'
BOY_COPY = '/Game/NPC/Companion/Appearance/SK_ErgouBoy'
EXPRESSIONS = '/Game/NPC/Companion/Appearance/DA_ErgouBoyExpressions'
CONVERSATION = '/Game/NPC/Residents/Animation/ABP_NpcConversation'
BOSS_SOURCE = '/Game/Characters/NPC/SK_NpcGrandpa'
BOSS_MESH = '/Game/NPC/Companion/Appearance/SK_NoodleOwner'
BOSS_SKELETON = '/Game/Characters/NPC/SK_NpcMan_Skeleton'
BOSS_ANIMATION = '/Game/NPC/Companion/Encounter/A_NoodleOwnerEject'
BAG_SOCKET = 'ErgouBagAnchor'
ANIMATIONS = ('idle', 'talk', 'walk', 'pace', 'hurry', 'hold', 'attack', 'down', 'get_up', 'hit')
MARKER = 'DeliveryProjectCompanionAppearance'
VERSION = '1'

def digest(file):
    return hashlib.sha256(Path(file).read_bytes()).hexdigest()


def object_path(value):
    return value.get_path_name() if value else None


def package_path(value):
    return value.split('.')[0]


def load(path, cls=None):
    value = unreal.load_asset(path)
    if not value or (cls and not isinstance(value, cls)):
        raise RuntimeError('缺少资产或类型错误：' + path)
    return value


def save(asset):
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError('保存失败：' + asset.get_path_name())


def compile_bp(bp):
    from editor_toolset.toolsets.blueprint import BlueprintTools
    BlueprintTools.compile_blueprint(bp)
    if bp.get_editor_property('status') != unreal.BlueprintStatus.BS_UP_TO_DATE:
        raise RuntimeError('蓝图没有成功编译：' + bp.get_path_name() + ' ' + str(bp.get_editor_property('status')))


def asset_file(project, path):
    return project / 'Content' / (package_path(path).removeprefix('/Game/') + '.uasset')


def backup(project, path, output):
    files = []
    for suffix in ('.uasset', '.uexp', '.ubulk'):
        source = asset_file(project, path).with_suffix(suffix)
        if source.exists():
            target = output / 'backup' / source.relative_to(project)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            if digest(source) != digest(target):
                raise RuntimeError('备份哈希不一致：' + str(source))
            files.append({'source': str(source), 'backup': str(target), 'sha256': digest(source)})
    return files


def graph_editor(bp, name):
    from editor_toolset.toolsets.blueprint import BlueprintTools
    return unreal.BlueprintGraphEditor.get_graph_editor(BlueprintTools.get_graph(bp, name))


def pin(node, name):
    matches = [p for p in node.list_all_pins() if str(p.get_pin_name()) == name]
    if len(matches) != 1:
        raise RuntimeError('引脚不唯一：' + node.get_name() + ':' + name)
    return matches[0]


def connected_nodes(start):
    found, pending = {}, [start]
    while pending:
        node = pending.pop()
        if node.get_path_name() in found:
            continue
        found[node.get_path_name()] = node
        for value in node.list_all_pins():
            pending.extend(link.get_owning_node() for link in value.list_connected_pins())
    return list(found.values())


def graph_snapshot(nodes):
    return {node.get_name(): [{'name': str(p.get_pin_name()), 'value': p.get_pin_value(),
                              'links': sorted(q.get_owning_node().get_name() + ':' + str(q.get_pin_name()) for q in p.list_connected_pins())}
                             for p in node.list_all_pins()] for node in nodes}


def remove_old_encounter(bp):
    from editor_toolset.toolsets.actor import ActorTools
    editor = graph_editor(bp, 'EventGraph')
    marker = unreal.EditorAssetLibrary.get_metadata_tag(bp, 'DeliveryCompanionEncounterGraph')
    event = editor.find_event_node('OnEncounterStarted')
    cdo = unreal.get_default_object(bp.generated_class())
    bosses = [c for c in ActorTools.get_components(cdo) if c.get_name() == 'NoodleOwner_GEN_VARIABLE']
    if marker == '3' and not event and not bosses:
        return {'status': 'already_removed'}
    if marker != '2' or not event or len(bosses) != 1:
        raise RuntimeError('旧演出图必须是已知 marker=2 且只有一个老板 SCS 组件')
    old = connected_nodes(event)
    signatures = [{str(p.get_pin_name()) for p in n.list_all_pins()} for n in old]
    # 编辑器工具不返回隐藏的 LatentInfo 引脚，Delay 按实际可见引脚核对
    required = [{'NewAnimToPlay', 'bLooping'}, {'InPos', 'bFireNotifies'}, {'execute', 'Duration', 'then'}, {'NoodleOwner'}]
    if len(old) != 11 or any(sum(required_pins.issubset(s) for s in signatures) != 1 for required_pins in required):
        raise RuntimeError('旧演出子图结构已变化，保留人工节点')
    play = next(n for n in old if 'NewAnimToPlay' in {str(p.get_pin_name()) for p in n.list_all_pins()})
    if pin(play, 'NewAnimToPlay').get_pin_value() != '/Game/NPC/Residents/Animation/A_ResidentPunch.A_ResidentPunch':
        raise RuntimeError('旧演出动画已由用户修改，停止自动删除')
    begin = editor.find_event_node('ReceiveBeginPlay')
    if not begin or len(connected_nodes(begin)) != 8:
        raise RuntimeError('背包 BeginPlay 图已变化，停止自动清理')
    before_bags = graph_snapshot(connected_nodes(begin))
    old_paths = {node.get_path_name() for node in old}
    if any(node.get_path_name() in old_paths for node in connected_nodes(begin)):
        raise RuntimeError('老板与背包图共享节点，停止删除')
    removed = graph_snapshot(old)
    # 先移除引用，再删 SCS；remove_component 内部会立即编译蓝图
    editor.remove_nodes(old)
    if graph_snapshot(connected_nodes(begin)) != before_bags:
        raise RuntimeError('删除旧演出时改变了背包图')
    if not ActorTools.remove_component(bosses[0]):
        raise RuntimeError('旧老板组件删除失败')
    unreal.EditorAssetLibrary.set_metadata_tag(bp, 'DeliveryCompanionEncounterGraph', '3')
    return {'status': 'removed', 'nodes': removed, 'bag_graph_preserved': True}


def preserved_companion(bp):
    cdo = unreal.get_default_object(bp.generated_class())
    profile = cdo.get_editor_property('profile')
    presentation = cdo.get_editor_property('presentation')
    return {'profile_table': object_path(profile.get_editor_property('data_table')),
            'profile_row': str(profile.get_editor_property('row_name')),
            'voice_set': object_path(presentation.get_editor_property('voice_set')),
            'tts_voice': str(presentation.get_editor_property('tts_voice')),
            'voice_frequency': presentation.get_editor_property('voice_frequency'),
            'behavior_tree': object_path(cdo.get_editor_property('behavior_tree')),
            'companion_tuning': object_path(cdo.get_editor_property('companion').get_editor_property('tuning'))}


def require_matching_skeleton(mesh, animation):
    if mesh.get_editor_property('skeleton') != animation.get_editor_property('skeleton'):
        raise RuntimeError('动画和模型使用不同骨架：' + animation.get_path_name())


def skeleton_info(mesh):
    modifier = unreal.SkeletonModifier()
    if not modifier.set_skeletal_mesh(mesh):
        raise RuntimeError('不能读取模型参考骨架：' + mesh.get_path_name())
    # 只查询，不调用 CommitSkeletonToSkeletalMesh，不生成世界 Actor
    return modifier, modifier.get_bone_transform('Head', True)


def unrotate(q, v):
    x, y, z, w = -q.x, -q.y, -q.z, q.w
    tx, ty, tz = 2*(y*v[2]-z*v[1]), 2*(z*v[0]-x*v[2]), 2*(x*v[1]-y*v[0])
    return [v[0]+w*tx+y*tz-z*ty, v[1]+w*ty+z*tx-x*tz, v[2]+w*tz+x*ty-y*tx]


def configure_boy_copy(cdo):
    source = load(BOY_SOURCE, unreal.SkeletalMesh)
    old_mesh = cdo.get_editor_property('mesh').get_editor_property('skeletal_mesh_asset')
    for key in ANIMATIONS:
        require_matching_skeleton(source, cdo.get_editor_property('presentation').get_editor_property(key + '_animation'))
    body = unreal.BlueprintEditorLibrary.get_blueprint_asset(cdo.get_editor_property('mesh').get_editor_property('anim_class'))
    if not body or body.get_editor_property('target_skeleton') != source.get_editor_property('skeleton'):
        raise RuntimeError('小男孩与当前 Body 蓝图的骨架不同')
    _, head = skeleton_info(source)
    forward, up = unrotate(head.rotation, (1, 0, 0)), unrotate(head.rotation, (0, 0, 1))
    conversation = load(CONVERSATION, unreal.AnimBlueprint)
    look_nodes = [n for n in graph_editor(conversation, 'AnimGraph').list_all_nodes() if n.get_class().get_name() == 'AnimGraphNode_LookAt']
    if len(look_nodes) != 1:
        raise RuntimeError('原注视图缺少唯一的 LookAt 节点')
    look = look_nodes[0].get_editor_property('node')
    expected = [look.get_editor_property(field).get_editor_property('axis') for field in ('look_at_axis', 'look_up_axis')]
    for actual, axis in zip((forward, up), expected):
        if max(abs(a-b) for a, b in zip(actual, (axis.x, axis.y, axis.z))) > .01:
            raise RuntimeError('小男孩头轴与现有注视图不同，需要独立适配后再绑定：' + str((forward, up)))
    mesh = unreal.load_asset(BOY_COPY)
    if not mesh:
        mesh = unreal.EditorAssetLibrary.duplicate_asset(BOY_SOURCE, BOY_COPY)
        if not mesh:
            raise RuntimeError('小男孩独立配置副本创建失败')
        mesh.set_editor_property('post_process_anim_blueprint', conversation.generated_class())
        unreal.EditorAssetLibrary.set_metadata_tag(mesh, MARKER, VERSION)
        save(mesh)
    elif unreal.EditorAssetLibrary.get_metadata_tag(mesh, MARKER) != VERSION:
        raise RuntimeError('已有小男孩副本未经本脚本标记，保留人工配置')
    expressions = unreal.load_asset(EXPRESSIONS)
    if not expressions:
        original = cdo.get_editor_property('presentation').get_editor_property('expressions')
        expressions = unreal.EditorAssetLibrary.duplicate_asset(original.get_path_name(), EXPRESSIONS)
        expressions.set_editor_property('poses', {})
        for field in ('blink_morph', 'look_left_morph', 'look_right_morph', 'look_up_morph', 'look_down_morph'):
            expressions.set_editor_property(field, 'None')
        unreal.EditorAssetLibrary.set_metadata_tag(expressions, MARKER, VERSION)
        save(expressions)
    elif unreal.EditorAssetLibrary.get_metadata_tag(expressions, MARKER) != VERSION:
        raise RuntimeError('已有二狗表情配置未经本脚本标记')
    return mesh, expressions, {'head_reference': str(head), 'forward_axis': forward, 'up_axis': up,
                               'previous_mesh': object_path(old_mesh), 'shared_body': object_path(body)}


def bind_boy(project, output):
    bp = load(COMPANION_BP, unreal.Blueprint)
    originals = backup(project, COMPANION_BP, output)
    before = preserved_companion(bp)
    cdo = unreal.get_default_object(bp.generated_class())
    boy, expressions, info = configure_boy_copy(cdo)
    configure_bag_anchor(boy, bp)
    old = remove_old_encounter(bp)
    # SCS 删除会编译，重新获取 CDO 与组件
    cdo = unreal.get_default_object(bp.generated_class())
    mesh = cdo.get_editor_property('mesh')
    mesh.set_skeletal_mesh_asset(boy)
    mesh.set_editor_property('override_materials', [])
    mesh.set_relative_scale3d(unreal.Vector(1, 1, 1))
    mesh.set_relative_rotation(unreal.Rotator(pitch=0, yaw=0, roll=0), False, False)
    bounds = boy.get_bounds()
    if abs(bounds.origin.z - bounds.box_extent.z) > .1 or abs(bounds.box_extent.z * 2 - 140) > .1:
        raise RuntimeError('小男孩尺寸不再是底0顶140cm，停止猜测缩放')
    mesh.set_relative_location(unreal.Vector(0, 0, -70), False, False)
    mesh.set_editor_property('disable_post_process_blueprint', False)
    cdo.get_editor_property('presentation').set_editor_property('expressions', expressions)
    capsule = cdo.get_editor_property('capsule_component')
    if capsule.get_unscaled_capsule_radius() != 30 or capsule.get_unscaled_capsule_half_height() != 70:
        raise RuntimeError('胶囊已发生非预期变化')
    compile_bp(bp)
    if preserved_companion(bp) != before:
        raise RuntimeError('人物 Profile、声音或行为树被意外改变，未保存角色蓝图')
    save(bp)
    return {'backups': originals, 'preserved': before, 'old_encounter': old, 'boy': info}


def configure_bag_anchor(mesh, bp):
    from editor_toolset.toolsets.skeletal_mesh import SkeletalMeshTools
    modifier, _ = skeleton_info(mesh)
    spine = modifier.get_bone_transform('Spine2', True)
    scale = spine.scale3d
    if min(scale.x, scale.y, scale.z) <= 0:
        raise RuntimeError('背包挂点骨骼缩放无效')
    if BAG_SOCKET not in map(str, SkeletalMeshTools.get_socket_names(mesh)):
        SkeletalMeshTools.add_socket(mesh, BAG_SOCKET, 'Spine2')
    socket = mesh.find_socket(BAG_SOCKET)
    socket.set_editor_property('relative_scale', unreal.Vector(1 / scale.x, 1 / scale.y, 1 / scale.z))
    socket.set_editor_property('relative_rotation', spine.rotation.inversed().rotator())
    socket.set_editor_property('relative_location', unreal.Vector())
    # 挂件自己的尺寸仍是厘米，不能再乘 Spine2 约140倍的导入缩放
    editor = graph_editor(bp, 'EventGraph')
    attachments = [node for node in editor.list_all_nodes()
                   if {'SocketName', 'ScaleRule', 'bWeldSimulatedBodies'}.issubset(
                       {str(p.get_pin_name()) for p in node.list_all_pins()})]
    if len(attachments) != 3:
        raise RuntimeError('预期三个背包挂接节点，保留人工图')
    for node in attachments:
        if not pin(node, 'SocketName').set_pin_value(BAG_SOCKET):
            raise RuntimeError('背包挂点赋值失败')
    save(mesh)


def import_boss_animation(project, repair_axes=False):
    # 老板使用项目卡通老人和居民同骨架动作，不再导入 The_Boss 的 FBX
    mesh = unreal.load_asset(BOSS_MESH)
    if not mesh:
        mesh = unreal.EditorAssetLibrary.duplicate_asset(BOSS_SOURCE, BOSS_MESH)
    if not mesh:
        raise RuntimeError('卡通老人独立副本创建失败')
    mesh.set_editor_property('post_process_anim_blueprint', None)
    save(mesh)
    animation = unreal.load_asset(BOSS_ANIMATION)
    if not animation:
        animation = unreal.EditorAssetLibrary.duplicate_asset('/Game/NPC/Residents/Animation/A_ResidentPunch', BOSS_ANIMATION)
    validate_boss_animation_source(animation)
    animation.set_editor_property('enable_root_motion', False)
    save(animation)
    return animation


def validate_boss_animation_source(animation):
    require_matching_skeleton(load(BOSS_MESH), animation)
    if animation.get_path_name().split('.')[0] != BOSS_ANIMATION:
        raise RuntimeError('老板应使用已核对的独立卡通老人动作')
    if not .1 < animation.get_play_length() < 3:
        raise RuntimeError('老板动作时长异常')


def bind_boss(project, output):
    animation_backup = backup(project, BOSS_ANIMATION, output)
    animation = import_boss_animation(project, '-RepairNoodleBossImportAxes' in unreal.SystemLibrary.get_command_line())
    bp = load(SCENE_BP, unreal.Blueprint)
    originals = backup(project, SCENE_BP, output)
    cdo = unreal.get_default_object(bp.generated_class())
    component = cdo.get_editor_property('noodle_owner')
    component.set_skeletal_mesh_asset(load(BOSS_MESH, unreal.SkeletalMesh))
    component.set_relative_rotation(unreal.Rotator(pitch=0, yaw=111.5502, roll=0), False, False)
    component.set_relative_scale3d(unreal.Vector(1, 1, 1))
    component.set_editor_property('override_materials', [])
    component.override_animation_data(animation, False, False, 0.0, 1.0)
    component.set_editor_property('disable_post_process_blueprint', True)
    cdo.set_editor_property('BossAnimationLength', animation.get_play_length())
    timing = calibrate_boss_timing(cdo, animation)
    compile_bp(bp)
    save(bp)
    return {'backups': originals, 'mesh': BOSS_MESH, 'animation': object_path(animation),
            'duration': animation.get_play_length(), 'transform': str(component.get_relative_transform()), 'timing': timing,
            'animation_backup': animation_backup,
            'animation_import_kind': animation.get_editor_property('asset_import_data').get_class().get_name()}


def calibrate_boss_timing(cdo, animation, apply=True):
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property('optional_skeletal_mesh', load(BOSS_MESH, unreal.SkeletalMesh))
    options.set_editor_property('evaluation_type', unreal.AnimDataEvalType.COMPRESSED)
    samples, references = [], {}
    for frame in range(round(animation.get_play_length() * 30) + 1):
        time = min(frame / 30, animation.get_play_length())
        pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(animation, time, options)
        positions = {}
        for bone in ('Hips', 'Head', 'Spine2', 'LeftHand', 'RightHand', 'LeftFoot', 'RightFoot'):
            position = unreal.AnimPoseExtensions.get_bone_pose(pose, bone, unreal.AnimPoseSpaces.WORLD).translation
            positions[bone] = [position.x, position.y, position.z]
            if frame == 0:
                reference = unreal.AnimPoseExtensions.get_ref_bone_pose(pose, bone, unreal.AnimPoseSpaces.WORLD).translation
                references[bone] = [reference.x, reference.y, reference.z]
        samples.append({'time': time, 'positions': positions})
    if any(sample['positions']['Head'][2] < 80 or min(sample['positions']['LeftFoot'][2], sample['positions']['RightFoot'][2]) < -20 for sample in samples):
        raise RuntimeError('老板动作高度或脚掌坐标错误：' + json.dumps({'reference': references, 'samples': samples}, ensure_ascii=False))
    # 卡通老人与居民共用 +X 正面，取手掌相对胸部前伸幅度最大的那只手
    hands = {}
    for bone in ('LeftHand', 'RightHand'):
        values = [sample['positions'][bone][0] - sample['positions']['Spine2'][0] for sample in samples]
        peak = values.index(max(values))
        hands[bone] = {'range_cm': max(values) - min(values), 'peak_cm': max(values), 'peak_time': samples[peak]['time']}
    hand = max(hands, key=lambda name: hands[name]['range_cm'])
    peak_time = hands[hand]['peak_time']
    door_duration = cdo.get_editor_property('door_travel_seconds')
    if peak_time <= 0 or hands[hand]['range_cm'] < 5 or door_duration <= 0:
        raise RuntimeError('老板动作没有可确认的向前挥赶峰值：' + str(hands))
    rate = max(1.0, peak_time / door_duration)
    delay = max(0.0, door_duration - peak_time / rate)
    if apply:
        cdo.set_editor_property('BossAnimationRate', rate)
        cdo.set_editor_property('BossAnimationDelay', delay)
    return {'door_travel_seconds': door_duration, 'hand': hand, 'hands': hands,
            'delay': delay, 'rate': rate, 'samples': samples, 'reference_positions': references,
            'note': '静态资产时间采样；开门阻挡时需由角色传入门实际进度对应的演出时间，不代表已实机验收'}


def inspect():
    from editor_toolset.toolsets.actor import ActorTools
    bp = load(COMPANION_BP, unreal.Blueprint)
    cdo = unreal.get_default_object(bp.generated_class())
    mesh = cdo.get_editor_property('mesh')
    editor = graph_editor(bp, 'EventGraph')
    result = {'companion_mesh': object_path(mesh.get_editor_property('skeletal_mesh_asset')),
              'material_overrides': [object_path(x) for x in mesh.get_editor_property('override_materials')],
              'mesh_transform': str(mesh.get_relative_transform()), 'body': object_path(mesh.get_editor_property('anim_class')),
              'post_process': object_path(mesh.get_editor_property('skeletal_mesh_asset').get_editor_property('post_process_anim_blueprint')),
              'blueprint_status': str(bp.get_editor_property('status')), 'preserved': preserved_companion(bp),
              'old_event_present': bool(editor.find_event_node('OnEncounterStarted')),
              'components': [object_path(c) for c in ActorTools.get_components(cdo)],
              'event_graph': graph_snapshot(editor.list_all_nodes()),
              'facial_limitations': ['项目小男孩没有形态键，尚无眨眼、眼球表情和嘴型驱动；注视与倾听身体姿态保留']}
    result['animations'] = {key: object_path(cdo.get_editor_property('presentation').get_editor_property(key + '_animation')) for key in ANIMATIONS}
    scene = load(SCENE_BP, unreal.Blueprint)
    scene_cdo = unreal.get_default_object(scene.generated_class())
    boss = scene_cdo.get_editor_property('noodle_owner')
    result['scene'] = {'status': str(scene.get_editor_property('status')), 'boss_mesh': object_path(boss.get_editor_property('skeletal_mesh_asset')),
                       'animation_data': str(boss.get_editor_property('animation_data')), 'transform': str(boss.get_relative_transform()),
                       'length': scene_cdo.get_editor_property('BossAnimationLength'),
                       'delay': scene_cdo.get_editor_property('BossAnimationDelay'),
                       'rate': scene_cdo.get_editor_property('BossAnimationRate')}
    animation = unreal.load_asset(BOSS_ANIMATION)
    if animation:
        imported = animation.get_editor_property('asset_import_data')
        result['boss_animation_import_kind'] = imported.get_class().get_name()
        result['boss_import_settings'] = {}
        for label, asset in [('animation', animation), ('source_mesh', load(BOSS_MESH))]:
            data = asset.get_editor_property('asset_import_data')
            entry = {'class': data.get_class().get_name()}
            if isinstance(data, unreal.InterchangeAssetImportData):
                entry['pipelines'] = []
                for pipeline in data.get_pipelines():
                    if isinstance(pipeline, unreal.InterchangeGenericAssetsPipeline):
                        entry['pipelines'].append({name: str(pipeline.get_editor_property(name)) for name in
                                                   ('ImportOffsetTranslation', 'ImportOffsetRotation', 'ImportOffsetUniformScale')})
                translator = data.get_translator_settings() or unreal.get_default_object(unreal.InterchangeFbxTranslatorSettings)
                entry['translator'] = {name: str(translator.get_editor_property(name)) for name in ('bConvertScene', 'bForceFrontXAxis', 'bConvertSceneUnit')}
            result['boss_import_settings'][label] = entry
        result['boss_animation_root_motion'] = animation.get_editor_property('enable_root_motion')
        result['boss_animation_timing'] = calibrate_boss_timing(scene_cdo, animation, False)
    if unreal.EditorAssetLibrary.get_metadata_tag(bp, 'DeliveryCompanionEncounterGraph') == '3':
        if result['old_event_present'] or any('NoodleOwner' in p for p in result['components']):
            raise RuntimeError('旧老板图或组件仍残留')
        if result['companion_mesh'] != BOY_COPY + '.SK_ErgouBoy' or result['material_overrides']:
            raise RuntimeError('新模型或材质覆盖读回不符')
        if bp.get_editor_property('status') != unreal.BlueprintStatus.BS_UP_TO_DATE:
            raise RuntimeError('角色蓝图读回不是已编译状态')
        begin = editor.find_event_node('ReceiveBeginPlay')
        if not begin or len(connected_nodes(begin)) != 8:
            raise RuntimeError('背包 BeginPlay 子图读回不符')
    return result


def main(bind_companion=False, bind_owner=False):
    project = Path(unreal.Paths.project_dir()).resolve()
    output = project / 'Saved/AIResearch/CompanionExistingAppearanceBinding' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('PIE 中不修改角色资产')
    protected = [project / 'Content/Level/TestForCharacter.umap']
    for folder in ('Characters/NPC', 'Characters/TestDeliveryMan', 'NPC/Residents', 'UI'):
        protected.extend((project / 'Content' / folder).rglob('*.uasset'))
    before = {str(file): digest(file) for file in protected if file.exists()}
    report = {'status': 'started', 'pie_started': False, 'map_saved': False}
    try:
        if bind_companion:
            report['companion_binding'] = bind_boy(project, output)
        if bind_owner:
            report['boss_binding'] = bind_boss(project, output)
        report['readback'] = inspect()
        report['status'] = 'saved_and_checked' if bind_companion or bind_owner else 'inspected'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        after = {str(file): digest(file) for file in protected if file.exists()}
        report.update(protected_before=before, protected_after=after, protected_unchanged=before == after)
        text = json.dumps(report, ensure_ascii=False, indent=2)
        (output / 'report.json').write_text(text, encoding='utf-8')
        (output.parent / 'latest.json').write_text(text, encoding='utf-8')
        unreal.log('COMPANION_EXISTING_BINDING_REPORT=' + str(output / 'report.json'))
        if before != after:
            raise RuntimeError('共享模型、街坊或地图哈希变化，停止后续集成')


if __name__ == '__main__':
    command = unreal.SystemLibrary.get_command_line()
    main('-BindExistingCompanionAppearance' in command, '-BindExistingNoodleBoss' in command)
