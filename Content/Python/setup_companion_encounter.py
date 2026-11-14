"""创建独立面馆初遇蓝图，只保存 BP_NoodleEncounter，不修改关卡

在完整编译 DeliveryEditor 后用 -run=pythonscript 执行
可选参数 -CompanionBossMesh=/Game/... -CompanionBossAnimation=/Game/...
已有脚本生成蓝图默认只读；显式 -UpdateCompanionBoss 才更新老板网格和匹配动画
-InspectCompanionScene 只读检查，不编译或保存
门、锚点、老板模型和动画数据保存在可编辑蓝图组件及变量中
"""
from datetime import datetime, timezone
import ast
import hashlib
import inspect as python_inspect
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil

import unreal


ROOT = '/Game/NPC/Companion/Encounter'
BP_PATH = ROOT + '/BP_NoodleEncounter'
DOOR_PATH = ROOT + '/SM_NoodleDoor'
MARKER = 'DeliveryNoodleEncounterGraph'
VERSION = '2'
SCENE_LOCATION = (-4577.9967686496775, -21011.84866558324, 3063.244666363056)
SCENE_YAW = 9.91312
THROW_START = (-4710.553114741846, -21159.523004395596, 3190.0)
THROW_LANDING = (-5250.315522979727, -20277.443672357927, 3037.604506757589)
TRIGGER = (-4753.591864741846, -20913.255504395598, 3190.0)
CLEARANCE_CENTER = (-151.44273993942699, -111.96578979492193, 405.0446490943432)
CLEARANCE_EXTENT = (172.33378837082353, 210.04443359375006, 414.5257854759693)


def path(value):
    return value.get_path_name() if value else None


def load(asset_path):
    result = unreal.load_asset(asset_path)
    if not result:
        raise RuntimeError('缺少资产：' + asset_path)
    return result


def vector(value):
    return unreal.Vector(*value)


def local_position(world_position):
    delta = [world_position[index] - SCENE_LOCATION[index] for index in range(3)]
    yaw = math.radians(SCENE_YAW)
    return unreal.Vector(math.cos(yaw) * delta[0] + math.sin(yaw) * delta[1],
                         -math.sin(yaw) * delta[0] + math.cos(yaw) * delta[1], delta[2])


def pin(node, name):
    matches = [item for item in node.list_all_pins() if str(item.get_pin_name()) == name]
    if len(matches) != 1:
        raise RuntimeError('蓝图引脚不唯一：' + node.get_name() + ':' + name)
    return matches[0]


def wire(left, output_name, right, input_name):
    if not pin(left, output_name).try_create_connection(pin(right, input_name)):
        raise RuntimeError('蓝图连线失败：' + output_name + ' -> ' + input_name)


def literal(node, name, value):
    if not pin(node, name).set_pin_value(str(value)):
        raise RuntimeError('蓝图引脚赋值失败：' + name + '=' + str(value))


def graph_tools(bp):
    from editor_toolset.toolsets.blueprint import BlueprintTools
    graph = BlueprintTools.get_graph(bp, 'EventGraph')
    return BlueprintTools, unreal.BlueprintGraphEditor.get_graph_editor(graph)


def create_graph(bp):
    tools, editor = graph_tools(bp)

    def call(function):
        result = editor.add_call_function_node(function)
        if not result:
            raise RuntimeError('不能创建蓝图函数节点：' + function)
        return result

    def member(name):
        result = editor.add_get_member_variable_node(name)
        if not result:
            raise RuntimeError('不能创建变量节点：' + name)
        return result

    for name, kind, category in [
            ('DoorClosedRotation', 'Rotator', '初遇|门'),
            ('DoorOpenRotation', 'Rotator', '初遇|门'),
            ('BossAnimationDelay', 'float', '初遇|老板'),
            ('BossAnimationRate', 'float', '初遇|老板'),
            ('BossAnimationLength', 'float', '初遇|老板')]:
        tools.add_variable(bp, name, kind)
        tools.set_variable_category(bp, name, category)
    tools.compile_blueprint(bp)
    # 新变量编译后重新取得图和 CDO，不能继续使用重建前的对象
    tools, editor = graph_tools(bp)

    door_event = tools.add_event(bp, 'ApplyDoorPresentation')
    door = member('DoorLeaf')
    closed = member('DoorClosedRotation')
    opened = member('DoorOpenRotation')
    rotate = call('/Script/Engine.KismetMathLibrary:RLerp')
    wire(closed, 'DoorClosedRotation', rotate, 'A')
    wire(opened, 'DoorOpenRotation', rotate, 'B')
    wire(door_event, 'Progress', rotate, 'Alpha')
    literal(rotate, 'bShortestPath', 'true')
    apply_rotation = call('/Script/Engine.SceneComponent:K2_SetRelativeRotation')
    wire(door_event, 'then', apply_rotation, 'execute')
    wire(door, 'DoorLeaf', apply_rotation, 'self')
    wire(rotate, 'ReturnValue', apply_rotation, 'NewRotation')
    # 服务器在每次推进前检查完整门叶范围，旋转节点自身没有碰撞扫掠能力
    literal(apply_rotation, 'bSweep', 'false')
    literal(apply_rotation, 'bTeleport', 'false')

    boss_event = tools.add_event(bp, 'UpdateBossPresentation')
    boss = member('NoodleOwner')
    hidden = call('/Script/Engine.KismetMathLibrary:Not_PreBool')
    wire(boss_event, 'bVisible', hidden, 'A')
    set_hidden = call('/Script/Engine.SceneComponent:SetHiddenInGame')
    wire(boss_event, 'then', set_hidden, 'execute')
    wire(boss, 'NoodleOwner', set_hidden, 'self')
    wire(hidden, 'ReturnValue', set_hidden, 'NewHidden')
    set_tick = call('/Script/Engine.ActorComponent:SetComponentTickEnabled')
    wire(set_hidden, 'then', set_tick, 'execute')
    wire(boss, 'NoodleOwner', set_tick, 'self')
    wire(boss_event, 'bVisible', set_tick, 'bEnabled')
    branch = editor.add_branch_node()
    wire(set_tick, 'then', branch, 'execute')
    wire(boss_event, 'bVisible', branch, 'Condition')
    delay = member('BossAnimationDelay')
    rate = member('BossAnimationRate')
    length = member('BossAnimationLength')
    elapsed = call('/Script/Engine.KismetMathLibrary:Subtract_DoubleDouble')
    wire(boss_event, 'EncounterElapsed', elapsed, 'A')
    wire(delay, 'BossAnimationDelay', elapsed, 'B')
    scaled = call('/Script/Engine.KismetMathLibrary:Multiply_DoubleDouble')
    wire(elapsed, 'ReturnValue', scaled, 'A')
    wire(rate, 'BossAnimationRate', scaled, 'B')
    clamp = call('/Script/Engine.KismetMathLibrary:FClamp')
    wire(scaled, 'ReturnValue', clamp, 'Value')
    literal(clamp, 'Min', 0)
    wire(length, 'BossAnimationLength', clamp, 'Max')
    seek = call('/Script/Engine.SkeletalMeshComponent:SetPosition')
    wire(branch, 'then', seek, 'execute')
    wire(boss, 'NoodleOwner', seek, 'self')
    wire(clamp, 'ReturnValue', seek, 'InPos')
    literal(seek, 'bFireNotifies', 'false')
    # 动画在组件 AnimationData 中配置并保持暂停，服务器经过时间是唯一播放位置来源
    tools.arrange_nodes(editor.list_all_nodes())
    tools.compile_blueprint(bp)


def configure_defaults(bp):
    cdo = unreal.get_default_object(bp.generated_class())
    cdo.set_editor_property('DoorClosedRotation', unreal.Rotator(0, 0, 0))
    cdo.set_editor_property('DoorOpenRotation', unreal.Rotator(pitch=0, yaw=90, roll=0))
    cdo.set_editor_property('BossAnimationDelay', 0.0)
    cdo.set_editor_property('BossAnimationRate', 1.0)
    cdo.set_editor_property('BossAnimationLength', 0.0)
    door = cdo.get_editor_property('door_leaf')
    door.set_static_mesh(load(DOOR_PATH))
    door.set_relative_location(unreal.Vector(), False, False)
    door.set_relative_rotation(unreal.Rotator(), False, False)
    door.set_relative_scale3d(unreal.Vector(1, 1, 1))
    door.set_collision_profile_name('BlockAllDynamic')
    door.set_editor_property('can_ever_affect_navigation', False)
    clearance = cdo.get_editor_property('doorway_clearance')
    clearance.set_relative_location(vector(CLEARANCE_CENTER), False, False)
    clearance.set_box_extent(vector(CLEARANCE_EXTENT), False)
    clearance.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    for field, location in [('trigger_anchor', TRIGGER), ('throw_start', THROW_START), ('throw_landing', THROW_LANDING)]:
        cdo.get_editor_property(field).set_relative_location(local_position(location), False, False)
    direction = [THROW_LANDING[index] - THROW_START[index] for index in range(3)]
    heading = math.degrees(math.atan2(direction[1], direction[0]))
    cdo.get_editor_property('throw_start').set_relative_rotation(unreal.Rotator(pitch=0, yaw=heading - SCENE_YAW, roll=0), False, False)
    boss = cdo.get_editor_property('noodle_owner')
    size = math.hypot(direction[0], direction[1])
    boss_location = (THROW_START[0] - direction[0] / size * 90,
                     THROW_START[1] - direction[1] / size * 90, SCENE_LOCATION[2] + 2)
    boss.set_relative_location(local_position(boss_location), False, False)
    boss.set_relative_rotation(unreal.Rotator(pitch=0, yaw=heading - SCENE_YAW - 90, roll=0), False, False)
    boss.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    boss.set_editor_property('can_ever_affect_navigation', False)
    boss.set_editor_property('hidden_in_game', True)
    boss.set_editor_property('disable_post_process_blueprint', True)


def configure_boss(bp, mesh_path, animation_path):
    if not mesh_path or not animation_path:
        raise RuntimeError('配置老板时必须同时提供独立网格和对应骨架的动画，没有默认替换成大壮')
    mesh, animation = load(mesh_path), load(animation_path)
    if not isinstance(mesh, unreal.SkeletalMesh) or not isinstance(animation, unreal.AnimSequence):
        raise RuntimeError('老板资源必须是 SkeletalMesh 与 AnimSequence')
    if mesh.get_editor_property('skeleton') != animation.get_editor_property('skeleton'):
        raise RuntimeError('老板网格和动画骨架不同，先完成动画重定向')
    helper_path = Path(__file__).with_name('import_companion_appearance.py')
    specification = importlib.util.spec_from_file_location('delivery_companion_appearance_helper', helper_path)
    helper = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(helper)
    if mesh.get_path_name().split('.')[0] != helper.BOSS_MESH:
        raise RuntimeError('当前使用已核对的项目卡通老人副本；其他模型需先完成独立姿势检查')
    helper.validate_boss_animation_source(animation)
    cdo = unreal.get_default_object(bp.generated_class())
    boss = cdo.get_editor_property('noodle_owner')
    boss.set_skeletal_mesh_asset(mesh)
    heading = math.degrees(math.atan2(THROW_LANDING[1] - THROW_START[1], THROW_LANDING[0] - THROW_START[0]))
    boss.set_relative_rotation(unreal.Rotator(pitch=0, yaw=heading - SCENE_YAW, roll=0), False, False)
    boss.set_relative_scale3d(unreal.Vector(1, 1, 1))
    boss.set_editor_property('override_materials', [])
    boss.override_animation_data(animation, False, False, 0.0, 1.0)
    cdo.set_editor_property('BossAnimationLength', animation.get_play_length())
    helper.calibrate_boss_timing(cdo, animation)


def repair_generated_v1_axes(bp, backup_dir):
    cdo = unreal.get_default_object(bp.generated_class())
    heading = math.degrees(math.atan2(THROW_LANDING[1] - THROW_START[1], THROW_LANDING[0] - THROW_START[0]))
    start = cdo.get_editor_property('throw_start')
    boss = cdo.get_editor_property('noodle_owner')
    opened = cdo.get_editor_property('DoorOpenRotation')
    if abs(opened.pitch - 90) > .001 or abs(opened.yaw) > .001 or abs(opened.roll) > .001:
        raise RuntimeError('v1门旋转已由用户调整，停止自动修正')
    for component, pitch in [(start, heading - SCENE_YAW), (boss, heading - SCENE_YAW - 90)]:
        # 组件会把超过90度的Pitch正规化成等价欧拉角，按四元数检查旧生成值
        rotation = component.get_relative_transform().rotation
        angle = math.radians(pitch) / 2
        dot = rotation.y * -math.sin(angle) + rotation.w * math.cos(angle)
        if abs(dot) < 1 - .00001 or abs(rotation.x) > .00001 or abs(rotation.z) > .00001:
            raise RuntimeError('v1场景旋转已由用户调整，停止自动修正')
    original = Path(unreal.Paths.project_content_dir()).resolve() / 'NPC/Companion/Encounter/BP_NoodleEncounter.uasset'
    shutil.copy2(original, backup_dir / 'BP_NoodleEncounter.before-axis-v2.uasset')
    # Python Rotator 的位置参数顺序不同于 C++，所有非零旋转都用字段名指定
    cdo.set_editor_property('DoorOpenRotation', unreal.Rotator(pitch=0, yaw=90, roll=0))
    start.set_relative_rotation(unreal.Rotator(pitch=0, yaw=heading - SCENE_YAW, roll=0), False, False)
    boss.set_relative_rotation(unreal.Rotator(pitch=0, yaw=heading - SCENE_YAW - 90, roll=0), False, False)


def actor_component_tools():
    import editor_toolset.toolsets.actor as actor_module
    from editor_toolset.toolsets.actor import ActorTools
    source_file = Path(actor_module.__file__).resolve()
    source = source_file.read_text(encoding='utf-8')
    definitions = {node.name: node for node in ast.walk(ast.parse(source)) if isinstance(node, ast.FunctionDef)}
    result = {}
    for name in ['get_components', 'remove_component', 'get_component_actor', 'set_parent_component']:
        fn = getattr(ActorTools, name)
        doc = python_inspect.getdoc(fn)
        try:
            signature = str(python_inspect.signature(fn))
        except (ValueError, TypeError):
            # UE 注册包装可能不公开 Python 签名，从本机工具源码读取，文档首句不是签名
            definition = definitions.get(name)
            signature = '(' + ast.unparse(definition.args) + ')' if definition else None
            if definition and definition.returns:
                signature += ' -> ' + ast.unparse(definition.returns)
        result[name] = {'signature': signature, 'doc': doc, 'source': str(source_file),
                        'source_line': definitions[name].lineno if name in definitions else None}
    previous = load('/Game/NPC/Companion/BP_Companion_Ergou')
    cdo = unreal.get_default_object(previous.generated_class())
    bosses = [component for component in ActorTools.get_components(cdo) if 'NoodleOwner' in component.get_name()]
    # marker=3 已删除旧 SCS 成员，CDO 上也不再存在同名属性
    result['old_boss_cdo_path'] = path(cdo.get_editor_property('NoodleOwner')) if bosses else None
    result['old_boss_component_templates'] = [path(component) for component in bosses]
    result['old_blueprint_status'] = str(previous.get_editor_property('status'))
    return result


def inspect(bp):
    tools, editor = graph_tools(bp)
    for event in ['ApplyDoorPresentation', 'UpdateBossPresentation']:
        if not editor.find_event_node(event):
            raise RuntimeError('场景蓝图缺少事件：' + event)
    nodes = []
    for node in editor.list_all_nodes():
        pins = [{'name': str(item.get_pin_name()), 'value': item.get_pin_value(),
                 'links': [linked.get_owning_node().get_name() + ':' + str(linked.get_pin_name())
                           for linked in item.list_connected_pins()]} for item in node.list_all_pins()]
        nodes.append({'name': node.get_name(), 'class': node.get_class().get_name(), 'pins': pins})
    seek = [item for item in nodes if {'InPos', 'bFireNotifies'}.issubset({value['name'] for value in item['pins']})]
    play = [item for item in nodes if {'NewAnimToPlay', 'bLooping'}.issubset({value['name'] for value in item['pins']})]
    delay = [item for item in nodes if {'LatentInfo', 'Duration'}.issubset({value['name'] for value in item['pins']})]
    if len(seek) != 1 or play or delay:
        raise RuntimeError('老板图必须只有一个时间定位节点，不能含逐帧重播或延迟节点')
    cdo = unreal.get_default_object(bp.generated_class())
    boss = cdo.get_editor_property('noodle_owner')
    return {'blueprint': bp.get_path_name(), 'parent': path(unreal.BlueprintEditorLibrary.get_blueprint_parent_class(bp)),
            'graph_version': unreal.EditorAssetLibrary.get_metadata_tag(bp, MARKER),
            'blueprint_status': str(bp.get_editor_property('status')), 'graph_nodes': nodes,
            'door_mesh': path(cdo.get_editor_property('door_leaf').get_editor_property('static_mesh')),
            'door_closed_rotation': str(cdo.get_editor_property('DoorClosedRotation')),
            'door_open_rotation': str(cdo.get_editor_property('DoorOpenRotation')),
            'doorway_clearance_center': str(cdo.get_editor_property('doorway_clearance').get_editor_property('relative_location')),
            'doorway_clearance_extent': str(cdo.get_editor_property('doorway_clearance').get_unscaled_box_extent()),
            'throw_start_relative': str(cdo.get_editor_property('throw_start').get_relative_transform()),
            'throw_landing_relative': str(cdo.get_editor_property('throw_landing').get_relative_transform()),
            'trigger_relative': str(cdo.get_editor_property('trigger_anchor').get_relative_transform()),
            'boss_mesh': path(boss.get_editor_property('skeletal_mesh_asset')),
            'boss_animation_data': str(boss.get_editor_property('animation_data')),
            'boss_relative_transform': str(boss.get_relative_transform()),
            'boss_animation_length': cdo.get_editor_property('BossAnimationLength'),
            'placement_reference': {'location': SCENE_LOCATION, 'yaw': SCENE_YAW, 'scale': [1, 1, 1]},
            'map_modified': False, 'pie_started': False}


def main(boss_mesh_path=None, boss_animation_path=None, inspect_only=False, update_boss=False):
    project = Path(unreal.Paths.project_dir()).resolve()
    output = project / 'Saved/AIResearch/CompanionSceneBlueprint' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=True)
    report = {'status': 'started', 'asset': BP_PATH}
    protected = [project / 'Content/Level/TestForCharacter.umap',
                 project / 'Content/model/buildings/shop1.uasset',
                 project / 'Content/NPC/Companion/BP_Companion_Ergou.uasset']
    before = {str(item): hashlib.sha256(item.read_bytes()).hexdigest() for item in protected}
    try:
        if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
            raise RuntimeError('PIE运行中禁止修改初遇资产')
        bp = unreal.load_asset(BP_PATH)
        created = not bool(bp)
        if inspect_only and created:
            raise RuntimeError('还没有独立场景蓝图可检查')
        marker = unreal.EditorAssetLibrary.get_metadata_tag(bp, MARKER) if bp else ''
        if bp and marker not in ('1', VERSION):
            raise RuntimeError('已有场景蓝图没有本脚本标记，保留人工资产')
        if bp and unreal.BlueprintEditorLibrary.get_blueprint_parent_class(bp) != unreal.DeliveryCompanionEncounterScene.static_class():
            raise RuntimeError('已有场景蓝图父类不同，停止修改')
        migrated = False
        if not inspect_only:
            if created:
                factory = unreal.BlueprintFactory()
                factory.set_editor_property('parent_class', unreal.DeliveryCompanionEncounterScene.static_class())
                bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset('BP_NoodleEncounter', ROOT, unreal.Blueprint, factory)
                if not bp:
                    raise RuntimeError('独立面馆场景蓝图创建失败')
                create_graph(bp)
                configure_defaults(bp)
                if boss_mesh_path or boss_animation_path:
                    configure_boss(bp, boss_mesh_path, boss_animation_path)
            elif marker == '1':
                repair_generated_v1_axes(bp, output)
                migrated = True
            if not created and update_boss:
                original = project / 'Content/NPC/Companion/Encounter/BP_NoodleEncounter.uasset'
                shutil.copy2(original, output / 'BP_NoodleEncounter.before.uasset')
                configure_boss(bp, boss_mesh_path, boss_animation_path)
            if created or update_boss or migrated:
                from editor_toolset.toolsets.blueprint import BlueprintTools
                BlueprintTools.compile_blueprint(bp)
                unreal.EditorAssetLibrary.set_metadata_tag(bp, MARKER, VERSION)
                if not unreal.EditorAssetLibrary.save_loaded_asset(bp):
                    raise RuntimeError('独立场景蓝图保存失败')
        report.update(migrated_v1_axes=migrated,
                      blueprint_compile_requested=not inspect_only and (created or update_boss or migrated))
        report.update(inspect(bp))
        report['actor_component_tools'] = actor_component_tools()
        report.update(status='inspected' if inspect_only else ('saved' if created or update_boss or migrated else 'unchanged'),
                      migrated_v1_axes=migrated,
                      blueprint_compile_requested=not inspect_only and (created or update_boss or migrated))
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        after = {str(item): hashlib.sha256(item.read_bytes()).hexdigest() for item in protected}
        report.update(protected_before=before, protected_after=after, protected_hashes_unchanged=before == after)
        text = json.dumps(report, ensure_ascii=False, indent=2)
        (output / 'report.json').write_text(text, encoding='utf-8')
        (output.parent / 'latest.json').write_text(text, encoding='utf-8')
        unreal.log('COMPANION_SCENE_BP_REPORT=' + str(output / 'report.json'))
        if before != after:
            raise RuntimeError('保护范围文件哈希变化，检查并停止后续集成')
    return report


if __name__ == '__main__':
    command_line = unreal.SystemLibrary.get_command_line()

    def argument(name):
        match = re.search(r'-' + name + r'=(?:"([^"]+)"|([^\s]+))', command_line)
        return (match.group(1) or match.group(2)) if match else None

    main(argument('CompanionBossMesh'), argument('CompanionBossAnimation'),
         '-InspectCompanionScene' in command_line, '-UpdateCompanionBoss' in command_line)
