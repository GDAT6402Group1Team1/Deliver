"""创建二狗第一阶段可编辑资产，并在第一单面馆旁布置初遇

编辑器运行：py place_companion.py
只读检查：命令行加 -InspectCompanionAssets
只迁移初遇图：命令行加 -MigrateCompanionEncounterOnly
生成场景需要完整编辑器，不能用 -run=pythonscript 复制导航体积
已有资产保留人工调整，只在首次创建时填入外观、声音与表现
现有外形与表情由 import_companion_appearance.py 配置，老板由独立面馆场景管理
首次创建要求已有二狗专属模型与表情配置，不恢复旧居民外形或随身老板
"""
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import shutil

import unreal


ROOT = '/Game/NPC/Companion'
MAP = '/Game/Level/TestForCharacter'
BP_PATH = ROOT + '/BP_Companion_Ergou'
TREE_PATH = ROOT + '/AI/BT_Companion'
PROFILE_PATH = ROOT + '/DT_CompanionProfiles'
TUNING_PATH = ROOT + '/DA_CompanionTuning'
VOICE_PATH = ROOT + '/DA_Voice_Ergou'
SOURCE_BP = '/Game/NPC/Residents/BP_ResidentLoud'
APPEARANCE_PATH = ROOT + '/Appearance/SK_ErgouBoy'
EXPRESSIONS_PATH = ROOT + '/Appearance/DA_ErgouBoyExpressions'
LABEL = 'NPC_Companion_Ergou'
TAG = 'DeliveryCompanionEncounter'
PERSONA = ('你叫牛二狗，是第一单面馆刚被开除的年轻帮厨，大家叫你二狗。'
           '你偷吃客人的汤面被老板赶出来，编织袋里养着毛毛虫小绿。'
           '你憨厚、忠心、认真干活，扶你起来的人是你认定的大哥或大姐，永远不改跟别人。'
           '你在村里的面馆干过活，认识的人和地点以背景与亲历记录为准，不补造每户资料。你怕狗和喇叭，对小绿说话很轻。'
           '像真人面对面说话，有自然的完整语气；不套口头禅、不故意听错话、不乱用成语，也不把自己写成傻子。'
           '笑点来自把小事看得认真、逞强和身体反应。平时自然陪伴；被问路时按已知地点回答，不凭空报方向或距离。'
           '语言安排由你理解，只选择当前真实可用的动作，不编造尚未完成的取件、送件或共同经历。'
           '其他人可以聊天，但不能替主人改变跟随、等候或跑腿安排。'
           '主人要求等候就认真等，召回就跟上，安静时少插话。失败原因以游戏结果为准。')


def load(path):
    value = unreal.load_asset(path)
    if not value:
        raise RuntimeError('缺少资产：' + path)
    return value


def path(value):
    return value.get_path_name() if value else None


def save(value):
    if not unreal.EditorAssetLibrary.save_loaded_asset(value):
        raise RuntimeError('资产保存失败：' + value.get_path_name())


def data_asset(asset_path, cls):
    existing = unreal.load_asset(asset_path)
    if existing:
        if not isinstance(existing, cls):
            raise RuntimeError('已有资产类型不同：' + asset_path)
        return existing
    folder, name = asset_path.rsplit('/', 1)
    factory = unreal.DataAssetFactory()
    factory.set_editor_property('data_asset_class', cls.static_class())
    result = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, cls, factory)
    if not result:
        raise RuntimeError('不能创建配置：' + asset_path)
    save(result)
    return result


def create_profile(voice):
    table = unreal.load_asset(PROFILE_PATH)
    row_struct = unreal.load_object(None, '/Script/Delivery.DeliveryNpcProfileRow')
    if not table:
        factory = unreal.DataTableFactory()
        factory.set_editor_property('struct', row_struct)
        table = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            'DT_CompanionProfiles', ROOT, unreal.DataTable, factory)
        rows = [{'Name': 'ergou', 'DisplayName': '牛二狗', 'Persona': PERSONA,
                 'TtsVoice': 'male', 'VoiceSet': path(voice)}]
        if not unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(
                table, json.dumps(rows, ensure_ascii=False), row_struct):
            raise RuntimeError('二狗人物表导入失败')
        save(table)
    if table.get_editor_property('row_struct') != row_struct:
        raise RuntimeError('二狗人物表结构不符')
    if 'ergou' not in map(str, unreal.DataTableFunctionLibrary.get_data_table_row_names(table)):
        raise RuntimeError('已有二狗人物表缺少 ergou 行，保留人工修改')
    return table


def create_material(name, color):
    material_path = ROOT + '/Appearance/' + name
    existing = unreal.load_asset(material_path)
    if existing:
        return existing
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, ROOT + '/Appearance', unreal.Material, unreal.MaterialFactoryNew())
    sample = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant3Vector, -280, 0)
    sample.set_editor_property('constant', unreal.LinearColor(*color, 1))
    unreal.MaterialEditingLibrary.connect_material_property(sample, '', unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(material)
    save(material)
    return material


def add_encounter_components(bp):
    from editor_toolset.toolsets.actor import ActorTools
    # 红白蓝三个软包形状保存在蓝图，正式编织袋到位后可以直接替换网格
    sphere = load('/Engine/BasicShapes/Sphere')
    for index, (name, color) in enumerate([
            ('Red', (.42, .035, .035)), ('White', (.65, .64, .58)), ('Blue', (.035, .09, .38))]):
        component = ActorTools.add_component(bp, unreal.StaticMeshComponent.static_class(), 'Bag' + name)
        if not component:
            raise RuntimeError('创建编织袋占位组件失败')
        # 添加蓝图组件会重编译，重新读取CDO以免保留上一轮的组件对象
        current = unreal.get_default_object(bp.generated_class())
        ActorTools.set_parent_component(component, current.get_editor_property('mesh'))
        component.set_static_mesh(sphere)
        component.set_material(0, create_material('M_Bag' + name, color))
        component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        component.set_relative_location(unreal.Vector(-17, (index - 1) * 9, -4), False, False)
        component.set_relative_scale3d(unreal.Vector(.18, .13, .38))
        component.set_editor_property('can_ever_affect_navigation', False)


def create_blueprint(table, tuning, tree):
    existing = unreal.load_asset(BP_PATH)
    if existing:
        if unreal.BlueprintEditorLibrary.get_blueprint_parent_class(existing) != unreal.DeliveryCompanionCharacter.static_class():
            raise RuntimeError('已有二狗蓝图父类不同，停止修改')
        return existing, False
    # 现有蓝图不重设外形；新建也只使用已准备的项目内二狗配置
    appearance = load(APPEARANCE_PATH)
    expressions = load(EXPRESSIONS_PATH)
    source = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(SOURCE_BP))
    source_mesh = source.get_editor_property('mesh')
    if appearance.get_editor_property('skeleton') != source_mesh.get_editor_property('skeletal_mesh_asset').get_editor_property('skeleton'):
        raise RuntimeError('二狗模型与复用动作骨架不同，先通过外形脚本完成适配')
    factory = unreal.BlueprintFactory()
    factory.set_editor_property('parent_class', unreal.DeliveryCompanionCharacter.static_class())
    bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'BP_Companion_Ergou', ROOT, unreal.Blueprint, factory)
    cdo = unreal.get_default_object(bp.generated_class())
    mesh = cdo.get_editor_property('mesh')
    mesh.set_skeletal_mesh_asset(appearance)
    mesh.set_editor_property('override_materials', [])
    mesh.set_relative_location(unreal.Vector(0, 0, -70), False, False)
    mesh.set_relative_rotation(unreal.Rotator(pitch=0, yaw=0, roll=0), False, False)
    mesh.set_relative_scale3d(unreal.Vector(1, 1, 1))
    mesh.set_editor_property('disable_post_process_blueprint', False)
    mesh.set_anim_instance_class(source_mesh.get_editor_property('anim_class'))
    cdo.get_editor_property('capsule_component').set_capsule_size(30, 70)
    presentation, previous = cdo.get_editor_property('presentation'), source.get_editor_property('presentation')
    for field in ['idle_animation', 'talk_animation', 'walk_animation', 'pace_animation',
                  'hurry_animation', 'hold_animation', 'attack_animation', 'down_animation',
                  'get_up_animation', 'hit_animation', 'voice_frequency',
                  'walk_animation_speed', 'pace_animation_speed', 'hurry_animation_speed']:
        presentation.set_editor_property(field, previous.get_editor_property(field))
    presentation.set_editor_property('expressions', expressions)
    handle = unreal.DataTableRowHandle()
    handle.set_editor_property('data_table', table)
    handle.set_editor_property('row_name', 'ergou')
    cdo.set_editor_property('profile', handle)
    cdo.set_editor_property('behavior_tree', tree)
    cdo.get_editor_property('companion').set_editor_property('tuning', tuning)
    if not cdo.apply_profile():
        raise RuntimeError('二狗人物表无法应用')
    add_encounter_components(bp)
    if not unreal.BlueprintEditorLibrary.compile_blueprint(bp):
        raise RuntimeError('二狗蓝图组件编译失败')
    save(bp)
    return bp, True


def create_encounter_graph(bp, backup_dir):
    from editor_toolset.toolsets.blueprint import BlueprintTools
    marker = unreal.EditorAssetLibrary.get_metadata_tag(bp, 'DeliveryCompanionEncounterGraph')
    if marker == '3':
        inspect_encounter_sync(bp)
        return 'unchanged_v3'
    if marker == '2':
        return 'unchanged_v2'
    graph = BlueprintTools.get_graph(bp, 'EventGraph')
    editor = unreal.BlueprintGraphEditor.get_graph_editor(graph)
    # 中文编辑器的节点菜单会本地化，按真实函数路径创建可编辑节点
    def pin(node, name):
        found = [p for p in node.list_all_pins() if str(p.get_pin_name()) == name]
        if len(found) != 1:
            raise RuntimeError('蓝图引脚不唯一：' + name + ' / ' + node.get_name())
        return found[0]

    def wire(left, output, right, input_name):
        if not pin(left, output).try_create_connection(pin(right, input_name)):
            raise RuntimeError('蓝图连线失败：' + output + ' -> ' + input_name)

    def literal(node, name, value):
        if not pin(node, name).set_pin_value(str(value)):
            raise RuntimeError('蓝图引脚值无效：' + name + '=' + str(value))

    def call(function):
        node = editor.add_call_function_node(function)
        if not node:
            raise RuntimeError('蓝图函数不存在：' + function)
        return node

    if marker == '1':
        # 只迁移本脚本生成的旧演出，按引脚与连线检查，不依赖本地化节点标题
        plays = [node for node in editor.list_all_nodes()
                 if {'NewAnimToPlay', 'bLooping', 'self', 'execute', 'then'}.issubset(
                     {str(p.get_pin_name()) for p in node.list_all_pins()})]
        if len(plays) != 1 or not editor.find_event_node('OnEncounterStarted'):
            raise RuntimeError('v1初遇图已经改动，停止自动迁移')
        play = plays[0]
        if 'A_ResidentPunch.A_ResidentPunch' not in pin(play, 'NewAnimToPlay').get_pin_value():
            raise RuntimeError('v1老板动画已经改动，停止自动迁移')

        def only_link(node, name):
            links = pin(node, name).list_connected_pins()
            if len(links) != 1:
                raise RuntimeError('v1初遇连线已经改动：' + name)
            return links[0]

        delay_input = only_link(play, 'then')
        delay = delay_input.get_owning_node()
        if str(delay_input.get_pin_name()) != 'execute':
            raise RuntimeError('v1老板动画后没有原等待节点')
        remaining = only_link(delay, 'Duration').get_owning_node()
        elapsed_output = only_link(remaining, 'B')
        boss_output = only_link(play, 'self')
        if str(boss_output.get_pin_name()) != 'NoodleOwner' or str(elapsed_output.get_pin_name()) != 'ReturnValue':
            raise RuntimeError('v1初遇时间或老板来源已经改动')
        if any('InPos' in {str(p.get_pin_name()) for p in node.list_all_pins()}
               for node in editor.list_all_nodes()):
            raise RuntimeError('v1初遇图已有自定义时间设置，保留用户节点')
        original = Path(unreal.Paths.project_content_dir()) / 'NPC/Companion/BP_Companion_Ergou.uasset'
        shutil.copy2(original, backup_dir / 'BP_Companion_Ergou.before-v2.uasset')
        seek = call('/Script/Engine.SkeletalMeshComponent:SetPosition')
        literal(seek, 'bFireNotifies', 'false')
        if not boss_output.try_create_connection(pin(seek, 'self')) or not elapsed_output.try_create_connection(pin(seek, 'InPos')):
            raise RuntimeError('不能把老板动画接到服务器演出时间')
        pin(play, 'then').break_single_pin_link(delay_input)
        wire(play, 'then', seek, 'execute')
        wire(seek, 'then', delay, 'execute')
        BlueprintTools.arrange_nodes(editor.list_all_nodes())
        if not unreal.BlueprintEditorLibrary.compile_blueprint(bp):
            raise RuntimeError('v2初遇表现图编译失败，未保存')
        unreal.EditorAssetLibrary.set_metadata_tag(bp, 'DeliveryCompanionEncounterGraph', '2')
        save(bp)
        return 'migrated_v1_to_v2'
    from editor_toolset.toolsets.actor import ActorTools
    has_old_boss = any(component.get_name() in ('NoodleOwner', 'NoodleOwner_GEN_VARIABLE')
                       for component in ActorTools.get_components(unreal.get_default_object(bp.generated_class())))
    if marker or has_old_boss or editor.find_event_node('OnEncounterStarted') or editor.find_event_node('ReceiveBeginPlay'):
        raise RuntimeError('已有自定义初遇图未标记，保留用户节点')

    begin = BlueprintTools.add_event(bp, 'ReceiveBeginPlay')
    mesh = editor.add_get_member_variable_node('Mesh')
    previous = begin
    for color in ['Red', 'White', 'Blue']:
        bag = editor.add_get_member_variable_node('Bag' + color)
        attach = call('/Script/Engine.SceneComponent:K2_AttachToComponent')
        wire(previous, 'then', attach, 'execute')
        wire(bag, 'Bag' + color, attach, 'self')
        wire(mesh, 'Mesh', attach, 'Parent')
        for name, value in [('SocketName', 'ErgouBagAnchor'), ('LocationRule', 'KeepRelative'),
                            ('RotationRule', 'KeepRelative'), ('ScaleRule', 'KeepRelative'),
                            ('bWeldSimulatedBodies', 'false')]:
            literal(attach, name, value)
        previous = attach
    # 老板已由独立场景接收服务器时间，角色只保留袋子的 BeginPlay 挂接
    BlueprintTools.arrange_nodes(editor.list_all_nodes())
    if not unreal.BlueprintEditorLibrary.compile_blueprint(bp):
        raise RuntimeError('二狗初遇表现图编译失败')
    unreal.EditorAssetLibrary.set_metadata_tag(bp, 'DeliveryCompanionEncounterGraph', '3')
    save(bp)
    return 'created_v3'


def transform(actor):
    value = actor.get_actor_transform()
    return {'position': [value.translation.x, value.translation.y, value.translation.z],
            'rotation': [value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w],
            'scale': [value.scale3d.x, value.scale3d.y, value.scale3d.z]}


def inspect_encounter_sync(bp):
    from editor_toolset.toolsets.blueprint import BlueprintTools
    graph = BlueprintTools.get_graph(bp, 'EventGraph')
    editor = unreal.BlueprintGraphEditor.get_graph_editor(graph)
    marker = unreal.EditorAssetLibrary.get_metadata_tag(bp, 'DeliveryCompanionEncounterGraph')
    if marker == '3':
        from editor_toolset.toolsets.actor import ActorTools
        cdo = unreal.get_default_object(bp.generated_class())
        if editor.find_event_node('OnEncounterStarted') or any(
                component.get_name() in ('NoodleOwner', 'NoodleOwner_GEN_VARIABLE')
                for component in ActorTools.get_components(cdo)):
            raise RuntimeError('v3二狗仍有旧老板组件或演出图，停止复建')
        begin = editor.find_event_node('ReceiveBeginPlay')
        if not begin:
            raise RuntimeError('v3二狗缺少背包 BeginPlay 图')
        connected, pending = set(), [begin]
        while pending:
            node = pending.pop()
            if node.get_path_name() in connected:
                continue
            connected.add(node.get_path_name())
            for item in node.list_all_pins():
                pending.extend(link.get_owning_node() for link in item.list_connected_pins())
        if len(connected) != 8:
            raise RuntimeError('v3背包 BeginPlay 图已变化，保留人工节点')
        return {'graph_version': marker, 'boss_owner': 'EncounterScene',
                'legacy_boss_event_present': False, 'bag_begin_play_nodes': len(connected),
                'scope': '只读检查角色图，独立场景绑定与老板参数见 carry_and_encounter.encounter_scene'}
    if marker != '2':
        raise RuntimeError('尚未支持的初遇图版本：' + marker)
    positions = [node for node in editor.list_all_nodes()
                 if {'InPos', 'bFireNotifies', 'self'}.issubset(
                     {str(p.get_pin_name()) for p in node.list_all_pins()})]
    if len(positions) != 1:
        raise RuntimeError('初遇图必须有且只有一个老板动画时间节点')
    seek = positions[0]

    def pin(node, name):
        return next(p for p in node.list_all_pins() if str(p.get_pin_name()) == name)

    def linked(node, name):
        links = pin(node, name).list_connected_pins()
        if len(links) != 1:
            raise RuntimeError('老板动画同步连线数量不符：' + name)
        return links[0]

    play = linked(seek, 'execute').get_owning_node()
    delay = linked(seek, 'then').get_owning_node()
    remaining = linked(delay, 'Duration').get_owning_node()
    elapsed = linked(remaining, 'B')
    boss = linked(seek, 'self')
    # 引脚包装对象每次查询都会重建，按所属节点与引脚名比较真实端点
    def endpoint(value):
        return value.get_owning_node().get_path_name() + ':' + str(value.get_pin_name())

    result = {
        'position_nodes': len(positions),
        'animation': pin(play, 'NewAnimToPlay').get_pin_value(),
        'target': str(boss.get_pin_name()),
        'position_source': endpoint(linked(seek, 'InPos')),
        'remaining_time_source': endpoint(elapsed),
        'position_uses_encounter_elapsed': endpoint(linked(seek, 'InPos')) == endpoint(elapsed),
        'fire_notifies': pin(seek, 'bFireNotifies').get_pin_value(),
        'play_then_position_then_delay': True,
    }
    if result['target'] != 'NoodleOwner' or not result['position_uses_encounter_elapsed'] or result['fire_notifies'].lower() != 'false':
        raise RuntimeError('老板动画同步读回不符：' + str(result))
    return result


def encounter_locations():
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()
    locations = [(actor, component) for actor in actors
                 for component in actor.get_components_by_class(unreal.DeliveryLocationComponent)
                 if str(component.get_editor_property('location_id')) == 'Location_Restaurant_001']
    if len(locations) != 1:
        raise RuntimeError('面馆地点必须唯一，实际数量：' + str(len(locations)))
    restaurant, marker = locations[0]
    starts = [a for a in actors if isinstance(a, unreal.PlayerStart)]
    if not starts:
        raise RuntimeError('没有 PlayerStart，停止猜测初遇路线')
    anchor = marker.get_world_location()
    start = min(starts, key=lambda a: (a.get_actor_location() - anchor).length())
    targets = [component for actor in actors
               for component in actor.get_components_by_class(unreal.DeliveryLocationComponent)
               if str(component.get_editor_property('location_id')) == 'Location_OldManHouse_001']
    if len(targets) != 1:
        raise RuntimeError('爷爷家的地点锚必须唯一')
    return restaurant, anchor, start, targets[0].get_world_location()


def ensure_navigation(world, report, inspect_only=False):
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    restaurant, anchor, start, door = encounter_locations()
    report.update(restaurant=restaurant.get_actor_label(),
                  pickup_anchor=[anchor.x, anchor.y, anchor.z], player_start=transform(start),
                  door_wait_anchor=[door.x, door.y, door.z], volume_changes=[])
    actors = subsystem.get_all_level_actors()
    existing = [a for a in actors if isinstance(a, unreal.NavMeshBoundsVolume)]
    source = next((a for a in existing if a.get_actor_label() == 'Resident_Navigation'), None)
    if not source:
        raise RuntimeError('缺少现有有效居民导航盒，无法复制独立同伴导航范围')
    _, extents = source.get_actor_bounds(False)
    if min(extents.x, extents.y, extents.z) < 100:
        raise RuntimeError('现有居民导航盒没有有效体积')
    source_scale = source.get_actor_scale3d()
    source_before = transform(source)
    # UE导航范围按世界轴对齐，分段盒避免一只旋转长盒覆盖过多无关区域
    points = [anchor, start.get_actor_location(), door]
    number = 0
    for first, last in zip(points, points[1:]):
        distance = (last - first).length()
        count = max(1, math.ceil(distance / 4000))
        for index in range(count):
            number += 1
            label = 'Companion_Navigation_%02d' % number
            a = first + (last - first) * (index / count)
            b = first + (last - first) * ((index + 1) / count)
            matches = [v for v in existing if v.get_actor_label() == label]
            if len(matches) > 1:
                raise RuntimeError('重复同伴导航范围：' + label)
            if matches:
                report['volume_changes'].append({'label': label, 'created': False, 'transform': transform(matches[0])})
                continue
            if inspect_only:
                raise RuntimeError('缺少同伴导航范围：' + label)
            center = (a + b) * .5
            size = unreal.Vector(abs(a.x - b.x) + 2400, abs(a.y - b.y) + 2400,
                                 abs(a.z - b.z) + 4000)
            volume = subsystem.duplicate_actor(source, world, center - source.get_actor_location())
            if not volume:
                raise RuntimeError('不能复制导航体积：' + label)
            volume.set_actor_label(label)
            volume.set_editor_property('tags', [unreal.Name('DeliveryCompanionNavigation')])
            volume.set_folder_path('NPC/Companion/Navigation')
            volume.set_actor_rotation(unreal.Rotator(0, 0, 0), False)
            volume.set_actor_scale3d(unreal.Vector(source_scale.x * size.x / (2 * extents.x),
                                                 source_scale.y * size.y / (2 * extents.y),
                                                 source_scale.z * size.z / (2 * extents.z)))
            report['volume_changes'].append({'label': label, 'created': True, 'transform': transform(volume)})
    if not inspect_only:
        # 现有编辑器辅助入口只重建导航数据，已有有效居民盒不移动也不改尺寸
        rebuilt = json.loads(unreal.DeliveryNpcToolset.check_npc_navigation(True))
        report['navigation_rebuild'] = rebuilt
        if rebuilt.get('status') != 'rebuilt_existing_bounds':
            raise RuntimeError('导航数据尚未可重建：' + rebuilt.get('status', 'unknown'))
    if source_before != transform(source):
        raise RuntimeError('原居民导航体积发生意外变化')
    report['resident_volume_unchanged'] = True


def place(world, blueprint):
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()
    restaurant, anchor, start, door = encounter_locations()
    toward = start.get_actor_location() - anchor
    toward.z = 0
    if toward.length() < 1:
        toward = restaurant.get_actor_forward_vector()
    toward = toward / toward.length()
    side = unreal.Vector(-toward.y, toward.x, 0)
    unreal.log('COMPANION_ENCOUNTER_ANCHOR ' + json.dumps({
        'restaurant': restaurant.get_actor_label(), 'pickup_anchor': [anchor.x, anchor.y, anchor.z],
        'player_start': transform(start), 'toward_start': [toward.x, toward.y, toward.z]}, ensure_ascii=False))
    found = [a for a in actors if TAG in map(str, a.tags)]
    if len(found) > 1:
        raise RuntimeError('存在重复二狗初遇Actor')
    created = not found
    yaw = math.degrees(math.atan2(toward.y, toward.x))
    if found:
        actor = found[0]
        if actor.get_class() != blueprint.generated_class():
            raise RuntimeError('二狗初遇标记指向其他类')
    else:
        # 沿玩家来路偏出取件交互区，地面与导航都有效后才生成
        point = None
        for sign in [1, -1]:
            candidate = anchor + toward * 320 + side * (260 * sign)
            hit = unreal.SystemLibrary.line_trace_single(
                world, candidate + unreal.Vector(0, 0, 400), candidate - unreal.Vector(0, 0, 2000),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [], unreal.DrawDebugTrace.NONE, True)
            if not hit or not hit.to_dict().get('blocking_hit'):
                continue
            ground = hit.to_dict()['impact_point']
            projected = unreal.NavigationSystemV1.project_point_to_navigation(
                world, ground, None, None, unreal.Vector(120, 120, 180))
            if projected is None or (projected - ground).length() > 180:
                continue
            point = projected + unreal.Vector(0, 0, 72)
            break
        if point is None:
            raise RuntimeError('面馆来路两侧没有确认可走的地面，未摆放二狗')
        actor = subsystem.spawn_actor_from_class(blueprint.generated_class(), point, unreal.Rotator(pitch=0, yaw=yaw, roll=0))
        actor.set_actor_label(LABEL)
        actor.set_editor_property('tags', [unreal.Name(TAG)])
        actor.set_folder_path('NPC/Companion')
    return {'created': created, 'actor': actor.get_path_name(), 'transform': transform(actor),
            'restaurant': restaurant.get_actor_label(), 'restaurant_transform': transform(restaurant),
            'pickup_anchor': [anchor.x, anchor.y, anchor.z], 'player_start': transform(start)}


def inspect_route(world, placement):
    actor = unreal.load_object(None, placement['actor'])
    _, pickup, start, door = encounter_locations()
    begin = actor.get_actor_location() - unreal.Vector(0, 0, 70)
    waypoints = [begin, start.get_actor_location(), door]
    projected = []
    for point in waypoints:
        value = unreal.NavigationSystemV1.project_point_to_navigation(
            world, point, None, None, unreal.Vector(180, 180, 400))
        if value is None:
            raise RuntimeError('第一单路线端点无法投影到导航：' + str(point))
        projected.append(value)
    results = []
    for first, last in zip(projected, projected[1:]):
        for reverse in [False, True]:
            a, b = (last, first) if reverse else (first, last)
            nav_path = unreal.NavigationSystemV1.find_path_to_location_synchronously(world, a, b, actor, None)
            valid = nav_path is not None and nav_path.is_valid() and not nav_path.is_partial()
            results.append({'start': [a.x, a.y, a.z], 'end': [b.x, b.y, b.z], 'reverse': reverse,
                            'complete': valid, 'points': len(nav_path.path_points) if nav_path else 0})
    # 门完整时院内可能不可达，单独记录而不以绕过门为通过条件
    inside = {'checked': False}
    residents = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    grandpa = next((a for a in residents if a.get_actor_label() == 'NPC_OldMan_001'), None)
    if grandpa:
        target = unreal.NavigationSystemV1.project_point_to_navigation(
            world, grandpa.get_actor_location(), None, None, unreal.Vector(80, 80, 150))
        inside = {'checked': True, 'projected': target is not None, 'complete': False}
        if target is not None:
            nav_path = unreal.NavigationSystemV1.find_path_to_location_synchronously(world, projected[-1], target, actor, None)
            inside['complete'] = nav_path is not None and nav_path.is_valid() and not nav_path.is_partial()
            inside['points'] = [[point.x, point.y, point.z] for point in nav_path.path_points] if nav_path else []
    return {'segments': results, 'complete': all(value['complete'] for value in results), 'grandpa_inside': inside,
            'scope': '编辑器静态路线：面馆初遇点—出生附近—爷爷门外，不验证游戏中的门口动态障碍和跟随效果'}


def inspect_carry_and_encounter(actor, cdo):
    # 只读取正式实例和继承组件，不编译蓝图或修改初遇配置
    def vector(value):
        return [value.x, value.y, value.z]

    def component_snapshot(component):
        rotation = component.get_editor_property('relative_rotation')
        return {'path': path(component), 'parent': path(component.get_attach_parent()),
                'socket': str(component.get_attach_socket_name()),
                'relative_location': vector(component.get_editor_property('relative_location')),
                'relative_rotation': [rotation.pitch, rotation.yaw, rotation.roll],
                'relative_scale': vector(component.get_editor_property('relative_scale3d')),
                'world_location': vector(component.get_world_location())}

    companion = actor.get_editor_property('companion')
    tuning = companion.get_editor_property('tuning')
    help_component = actor.get_editor_property('help_interactable')
    interface = unreal.load_class(None, '/Script/Delivery.DeliveryPackageCarrierInterface')
    item_defaults = unreal.get_default_object(unreal.DeliveryItemComponent.static_class())
    result = {
        'scope': '编辑器静态资产读回，接口声明不等于交接或网络运行验证',
        'actor_replicates': actor.get_editor_property('replicates'),
        'companion_replicates': companion.get_editor_property('replicates'),
        'package_item_component_replicates': item_defaults.get_editor_property('replicates'),
        'implements_package_carrier': unreal.SystemLibrary.does_implement_interface(actor, interface),
        'owner_player_state': path(companion.get_owner_player_state()),
        'mode': str(companion.get_mode()),
        'package_anchor_cdo': component_snapshot(cdo.get_editor_property('package_anchor')),
        'package_anchor_instance': component_snapshot(actor.get_editor_property('package_anchor')),
        'encounter_task_id': str(actor.get_editor_property('encounter_task_id')),
        'help_interaction': {name: str(help_component.get_editor_property(name))
                             for name in ('interaction_key', 'interact_radius', 'hold_duration', 'prompt_text')},
        'tuning_values': {name: tuning.get_editor_property(name) for name in
                          ('encounter_radius', 'help_hold_seconds', 'help_recovery_seconds', 'lead_distance',
                           'side_distance', 'destination_clearance', 'wait_yield_radius', 'catch_up_distance')},
        'encounter_components': {},
    }
    presentation = actor.get_editor_property('presentation')
    animations = {name: presentation.get_editor_property(name) for name in
                  ('idle_animation', 'walk_animation', 'down_animation', 'get_up_animation', 'attack_animation')}
    scene = actor.get_editor_property('encounter_scene')
    result['encounter_scene'] = {'reference': path(scene), 'configured': scene is not None}
    if scene:
        if not isinstance(scene, unreal.DeliveryCompanionEncounterScene):
            raise RuntimeError('二狗当前场景引用类型不符，保留原绑定')
        boss = scene.get_editor_property('noodle_owner')
        animation_data = boss.get_editor_property('animation_data')
        animations['noodle_owner_scene_animation'] = animation_data.get_editor_property('anim_to_play')
        result['encounter_scene'].update(
            actor_class=path(scene.get_class()), transform=transform(scene),
            boss={**component_snapshot(boss),
                  'mesh': path(boss.get_editor_property('skeletal_mesh_asset')),
                  'animation_data': str(animation_data),
                  'visible': boss.get_editor_property('visible'),
                  'hidden_in_game': boss.get_editor_property('hidden_in_game')},
            door=component_snapshot(scene.get_editor_property('door_leaf')),
            throw_start=component_snapshot(scene.get_editor_property('throw_start')),
            throw_landing=component_snapshot(scene.get_editor_property('throw_landing')))
    else:
        result['encounter_scene']['limitation'] = '当前实例尚未绑定独立面馆场景，本脚本不猜测或修改引用'
    result['animations'] = {name: {'asset': path(animation),
                                   'length_seconds': animation.get_editor_property('sequence_length') if animation else None}
                            for name, animation in animations.items()}
    for component in actor.get_components_by_class(unreal.SceneComponent):
        if component.get_name() not in ('NoodleOwner', 'BagRed', 'BagWhite', 'BagBlue'):
            continue
        item = component_snapshot(component)
        item.update(visible=component.get_editor_property('visible'),
                    hidden_in_game=component.get_editor_property('hidden_in_game'))
        if isinstance(component, unreal.SkeletalMeshComponent):
            item['mesh'] = path(component.get_editor_property('skeletal_mesh_asset'))
        elif isinstance(component, unreal.StaticMeshComponent):
            item['mesh'] = path(component.get_editor_property('static_mesh'))
        result['encounter_components'][component.get_name()] = item
    restaurant, anchor, player_start, _ = encounter_locations()
    result['restaurant_actor'] = {'path': path(restaurant), 'transform': transform(restaurant)}
    result['restaurant_components'] = [component_snapshot(component)
                                       for component in restaurant.get_components_by_class(unreal.SceneComponent)]
    result['pickup_anchor'] = vector(anchor)
    result['nearest_player_start'] = transform(player_start)
    return result


def main(inspect_only=False, encounter_only=False):
    project = Path(unreal.Paths.project_dir()).resolve()
    output = project / 'Saved/AIResearch/CompanionAssets' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'preparing', 'inspect_only': inspect_only,
              'limitations': ['现有外形、表情、声音及独立场景参数保持原配置，编织袋仍为占位',
                              '未运行PIE或实际交互，不证明初遇、跟随和多人表现已验收']}
    try:
        if inspect_only and encounter_only:
            raise RuntimeError('只读检查与初遇图迁移必须分开执行')
        if not inspect_only and '-run=pythonscript' in unreal.SystemLibrary.get_command_line().lower():
            raise RuntimeError('导航体积复制需要完整编辑器，请在地图加载完成后运行脚本，不要使用命令行Commandlet')
        if not hasattr(unreal, 'DeliveryCompanionCharacter'):
            raise RuntimeError('先完整编译 DeliveryEditor，再生成二狗资产')
        if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
            raise RuntimeError('PIE运行中禁止创建或保存同伴资产')
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        if not world or world.get_path_name().split('.')[0] != MAP:
            world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
        if not world:
            raise RuntimeError('正式关卡加载失败')
        if encounter_only:
            bp = load(BP_PATH)
            report['encounter_graph'] = create_encounter_graph(bp, output)
            report['modified_scope'] = [] if report['encounter_graph'].startswith('unchanged_') else [BP_PATH]
            actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
            placed = [actor for actor in actors if TAG in map(str, actor.tags)]
            if len(placed) != 1:
                raise RuntimeError('初遇Actor数量不符')
            report['placement'] = {'created': False, 'actor': placed[0].get_path_name(),
                                   'transform': transform(placed[0])}
        elif not inspect_only:
            result = unreal.DeliveryNpcToolset.create_companion_behavior_asset()
            report['behavior_tool'] = result
            if result not in ('Saved BT_Companion and BTT_CompanionFollow', 'BT_Companion already configured'):
                raise RuntimeError('同伴行为树未完成：' + result)
            tree = load(TREE_PATH)
            tuning = data_asset(TUNING_PATH, unreal.DeliveryCompanionTuning)
            voice = unreal.load_asset(VOICE_PATH)
            if not voice:
                voice = unreal.EditorAssetLibrary.duplicate_asset('/Game/NPC/Residents/Audio/DA_Voice_OldMan', VOICE_PATH)
                save(voice)
            table = create_profile(voice)
            bp, report['created_blueprint'] = create_blueprint(table, tuning, tree)
            report['encounter_graph'] = create_encounter_graph(bp, output)
            actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
            before = {a.get_path_name(): transform(a) for a in actors if TAG not in map(str, a.tags)}
            report['navigation'] = {}
            ensure_navigation(world, report['navigation'])
            report['placement'] = place(world, bp)
            report['route'] = inspect_route(world, report['placement'])
            if not report['route']['complete']:
                raise RuntimeError('面馆到爷爷门外仍没有完整导航路径，地图未保存')
            after = {a.get_path_name(): transform(a) for a in actors if TAG not in map(str, a.tags)}
            if before != after:
                raise RuntimeError('已有场景物件发生位移，地图未保存')
            changed = (report['placement']['created']
                       or any(v['created'] for v in report['navigation']['volume_changes']))
            if changed:
                original = project / 'Content/Level/TestForCharacter.umap'
                shutil.copy2(original, output / 'TestForCharacter.before.umap')
                report['map_before_sha256'] = hashlib.sha256(original.read_bytes()).hexdigest()
            if changed and not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
                raise RuntimeError('二狗初遇地图保存失败')
            report['other_actor_transforms_unchanged'] = True
        else:
            report['navigation'] = {}
            ensure_navigation(world, report['navigation'], True)
            actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
            placed = [actor for actor in actors if TAG in map(str, actor.tags)]
            if len(placed) != 1:
                raise RuntimeError('重载后初遇Actor数量不符')
            rotation = placed[0].get_actor_rotation()
            if abs(rotation.pitch) > .1 or abs(rotation.roll) > .1:
                raise RuntimeError('二狗初遇Actor仍有倾斜')
            report['placement'] = {'created': False, 'actor': placed[0].get_path_name(),
                                   'transform': transform(placed[0])}
            report['route'] = inspect_route(world, report['placement'])
            if not report['route']['complete']:
                raise RuntimeError('重载后的第一单路线不完整')
        bp = load(BP_PATH)
        report['encounter_graph_version'] = unreal.EditorAssetLibrary.get_metadata_tag(bp, 'DeliveryCompanionEncounterGraph')
        report['encounter_sync'] = inspect_encounter_sync(bp)
        # 独立读回不重编蓝图，避免重建当前关卡实例及其编辑器渲染状态
        report['blueprint_status'] = str(bp.get_editor_property('status'))
        report['blueprint_status_is_up_to_date'] = 'UP_TO_DATE' in report['blueprint_status'].upper()
        if inspect_only:
            report['blueprint_compile_requested'] = False
        cdo = unreal.get_default_object(bp.generated_class())
        report['parent_class'] = path(unreal.BlueprintEditorLibrary.get_blueprint_parent_class(bp))
        report['behavior_tree'] = path(cdo.get_editor_property('behavior_tree'))
        report['tuning'] = path(cdo.get_editor_property('companion').get_editor_property('tuning'))
        report['mesh'] = path(cdo.get_editor_property('mesh').get_editor_property('skeletal_mesh_asset'))
        report['animation'] = path(cdo.get_editor_property('mesh').get_editor_property('anim_class'))
        report['profile'] = path(cdo.get_editor_property('profile').get_editor_property('data_table'))
        report['tts_voice'] = str(cdo.get_editor_property('presentation').get_editor_property('tts_voice'))
        report['expressions'] = path(cdo.get_editor_property('presentation').get_editor_property('expressions'))
        placed_actor = unreal.load_object(None, report['placement']['actor'])
        placed_mesh = placed_actor.get_editor_property('mesh')
        report['instance_mesh'] = {
            'mesh': path(placed_mesh.get_editor_property('skeletal_mesh_asset')),
            'animation': path(placed_mesh.get_editor_property('anim_class')),
            'visible': placed_mesh.get_editor_property('visible'),
            'hidden_in_game': placed_mesh.get_editor_property('hidden_in_game'),
            'relative_transform': str(placed_mesh.get_relative_transform()),
            'material_overrides': [path(material) for material in placed_mesh.get_editor_property('override_materials')],
            'expressions': path(placed_actor.get_editor_property('presentation').get_editor_property('expressions')),
        }
        report['carry_and_encounter'] = inspect_carry_and_encounter(placed_actor, cdo)
        if not report['blueprint_status_is_up_to_date'] or report['behavior_tree'] != TREE_PATH + '.BT_Companion':
            raise RuntimeError('二狗蓝图或行为树绑定检查失败')
        report['status'] = 'inspected' if inspect_only else 'saved'
        if encounter_only and not report['modified_scope']:
            report['status'] = 'unchanged'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        text = json.dumps(report, ensure_ascii=False, indent=2)
        (output / 'report.json').write_text(text, encoding='utf-8')
        (output.parent / 'latest.json').write_text(text, encoding='utf-8')
        unreal.log('COMPANION_ASSETS_REPORT=' + str(output / 'report.json'))


COMPANION_PERSONA_OLD_SHA256 = '88ec348fb67ff69851234cd2d7253a7875ff77b9b9cd01534594faeef7e763a0'
COMPANION_PERSONA_NEW_SHA256 = '2c1c54d87b868cb66fd83fbb9cf986a8629f6747778696f69891a57ba2295329'


def update_companion_persona_only(verify_only=False):
    """仅迁移 ergou.Persona；verify_only 在独立进程重读上次保存记录"""
    import copy
    import os
    import re
    import runpy
    import subprocess
    import sys

    project = Path(unreal.Paths.project_dir()).resolve()
    if '-run=pythonscript' not in unreal.SystemLibrary.get_command_line().lower():
        raise RuntimeError('单表迁移和独立读回只在 Python Commandlet 中执行')
    helper_file = project/'Content/Python/update_npc_personas.py'
    ensure_editor_closed = runpy.run_path(str(helper_file))['ensure_editor_closed']

    def guard_processes():
        ensure_editor_closed()
        if sys.platform != 'win32':
            raise RuntimeError('本次资产迁移的进程守门只针对已核验的 Windows 环境')
        # Commandlet 也可能并行写资源，排除本进程后拒绝其他同项目 UE/UBT
        command = ("Get-CimInstance Win32_Process | Where-Object { $_.Name -in "
                   "@('UnrealEditor.exe','UnrealEditor-Cmd.exe','UnrealBuildTool.exe','dotnet.exe') } | "
                   "Select-Object ProcessId,Name,CommandLine | ConvertTo-Json -Compress")
        result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', command],
                                capture_output=True, text=True, check=True)
        processes = json.loads(result.stdout) if result.stdout.strip() else []
        if isinstance(processes, dict):
            processes = [processes]
        for item in processes:
            if int(item['ProcessId']) == os.getpid():
                continue
            line = item.get('CommandLine')
            if item['Name'].lower() == 'dotnet.exe' and line is not None and 'unrealbuildtool' not in line.lower():
                continue
            if line is None or 'delivery.uproject' in line.lower():
                raise RuntimeError('另一个 UE/构建进程占用项目或无法核实其项目：'+str(item['ProcessId']))

    def digest(value):
        return hashlib.sha256(value.encode('utf-8')).hexdigest() if isinstance(value, str) else ''

    def file_hash(file):
        value = hashlib.sha256()
        with file.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1024*1024), b''):
                value.update(chunk)
        return value.hexdigest()

    def dirty_packages():
        utility = unreal.EditorLoadingAndSavingUtils
        return sorted(p.get_path_name() for p in list(utility.get_dirty_map_packages())+list(utility.get_dirty_content_packages()))

    def export_rows(table, filename):
        if filename.exists() or not unreal.DataTableFunctionLibrary.export_data_table_to_json_file(table, str(filename)):
            raise RuntimeError('人物表导出失败或输出文件已存在')
        rows = json.loads(filename.read_text(encoding='utf-8-sig'))
        if not isinstance(rows, list) or not all(isinstance(row, dict) and isinstance(row.get('Name'), str) for row in rows):
            raise RuntimeError('人物表导出结构无效')
        names = [row['Name'] for row in rows]
        if len(set(names)) != len(names) or names.count('ergou') != 1:
            raise RuntimeError('人物表缺少唯一 ergou 行或存在重复行')
        return rows

    package_extensions = ('.uasset', '.uexp', '.ubulk', '.uptnl')
    table_file = project/'Content/NPC/Companion/DT_CompanionProfiles.uasset'
    table_files = [table_file.with_suffix(ext) for ext in package_extensions]
    allowed_paths = {str(p.resolve()) for p in table_files}
    extra_files = {project/'Content/Level/TestForCharacter.umap',
                   project/'Content/NPC/Residents/Config/DT_NpcProfiles.uasset',
                   project/'Content/NPC/Residents/BP_ResidentLoud.uasset',
                   project/'Content/NPC/Residents/BP_ResidentShrewd.uasset'}

    def disk_snapshot():
        files = set(extra_files)
        for folder in (project/'Content/NPC/Companion', project/'Content/NPC/Residents/Audio'):
            files.update(p for p in folder.rglob('*') if p.is_file() and p.suffix.lower() in package_extensions)
        # 固定包路径也记录缺失，不能把新建/消失的包漏成“原文件没有变化”
        files.update(table_files)
        return {str(p.resolve()): file_hash(p) if p.exists() else None for p in sorted(files)}

    guard_processes()
    if digest(PERSONA) != COMPANION_PERSONA_NEW_SHA256 or not 0 < len(PERSONA) <= 1024:
        raise RuntimeError('脚本中的目标 Persona 已改变，必须先审查新文本')
    if not table_file.is_file():
        raise RuntimeError('正式人物表不存在，迁移不会调用 create_profile 创建新表')
    output_root = project/'Saved/AIResearch/CompanionPersonaUpdate'
    output = output_root/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {'schema_version': 1, 'status': 'started', 'verify_only': verify_only, 'pid': os.getpid(),
        'table': PROFILE_PATH, 'folder': str(output), 'allowed_change': 'ergou.Persona only',
        'script_sha256': file_hash(Path(__file__)), 'process_guard_helper_sha256': file_hash(helper_file),
        'old_persona_sha256': COMPANION_PERSONA_OLD_SHA256, 'new_persona_sha256': COMPANION_PERSONA_NEW_SHA256,
        'saved_assets': [], 'backups': {}, 'explicitly_not_loaded': ['map', 'NPC Blueprint', 'ApplyProfile'],
        'hash_scope': 'Absolute package paths: all Companion packages, resident audio packages, both resident BPs/profile table, formal map and all row asset references'}
    table, before, expected = None, None, None
    save_attempted = False

    def write_report():
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    try:
        report['dirty_before'] = dirty_packages()
        if report['dirty_before']:
            raise RuntimeError('启动时存在脏包，停止单表迁移/读回')
        table = unreal.load_asset(PROFILE_PATH)
        row_struct = unreal.load_object(None, '/Script/Delivery.DeliveryNpcProfileRow')
        if not isinstance(table, unreal.DataTable) or table.get_editor_property('row_struct') != row_struct:
            raise RuntimeError('正式表类型或行类型不符')
        if dirty_packages():
            raise RuntimeError('仅加载人物表已产生脏包，停止写入')
        before = export_rows(table, output/'before.json')
        old = next(row for row in before if row['Name'] == 'ergou')
        if digest(old.get('Persona')) not in (COMPANION_PERSONA_OLD_SHA256, COMPANION_PERSONA_NEW_SHA256):
            raise RuntimeError('正式 ergou.Persona 不是已确认旧值或新值，拒绝覆盖人工修改')
        # 声音和其它对象引用也按真实导出路径保护，不加载这些资源
        for match in re.finditer(r'/Game/[A-Za-z0-9_/-]+(?:\.[A-Za-z0-9_]+)?', json.dumps(before, ensure_ascii=False)):
            package = match[0].split('.', 1)[0]
            file = (project/'Content'/package[len('/Game/'):]).with_suffix('.uasset')
            if not file.resolve().is_relative_to((project/'Content').resolve()):
                raise RuntimeError('表中的资产路径不在 Content 中')
            extra_files.update(file.with_suffix(ext) for ext in package_extensions)
        report['files_before'] = disk_snapshot()
        expected = copy.deepcopy(before)
        next(row for row in expected if row['Name'] == 'ergou')['Persona'] = PERSONA
        (output/'expected.json').write_text(json.dumps(expected, ensure_ascii=False, indent=2), encoding='utf-8')
        report['before_persona_sha256'] = digest(old['Persona'])
        if verify_only:
            latest = output_root/'latest.json'
            saved = json.loads(latest.read_text(encoding='utf-8'))
            if saved.get('status') not in ('saved', 'already_current') or saved.get('table') != PROFILE_PATH or saved.get('pid') == os.getpid():
                raise RuntimeError('独立读回必须对照另一个进程的成功保存记录')
            previous = Path(saved['folder']).resolve()
            if not previous.is_relative_to(output_root.resolve()):
                raise RuntimeError('保存记录目录越界')
            after_file = previous/'after.json'
            if file_hash(after_file) != saved['after_json_sha256']:
                raise RuntimeError('保存时的整表导出被修改')
            saved_rows = json.loads(after_file.read_text(encoding='utf-8-sig'))
            if before != expected or before != saved_rows or report['files_before'] != saved['files_after']:
                raise RuntimeError('独立重载的表、声音/其它字段或受保护资源与保存记录不同')
            report.update(status='read_back_verified', verified_saved_report=str(previous/'report.json'))
        else:
            for file in table_files:
                if file.exists():
                    backup = output/'Backup'/'Content/NPC/Companion'/file.name
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(file, backup)
                    if file_hash(backup) != report['files_before'][str(file.resolve())]:
                        raise RuntimeError('正式表备份哈希不符')
                    report['backups'][str(file.resolve())] = {'path': str(backup), 'sha256': file_hash(backup)}
            write_report()
            if before != expected:
                guard_processes()
                if disk_snapshot() != report['files_before'] or dirty_packages():
                    raise RuntimeError('导出后磁盘资源或脏包状态变化，停止写入')
                table.modify()
                if not unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(expected, ensure_ascii=False), row_struct):
                    raise RuntimeError('人物表导入失败')
                if export_rows(table, output/'checked-before-save.json') != expected:
                    raise RuntimeError('除 ergou.Persona 外还有字段或行发生变化，停止保存')
                if set(dirty_packages())-set([PROFILE_PATH]) or disk_snapshot() != report['files_before']:
                    raise RuntimeError('导入期间影响其他包或磁盘文件，停止保存')
                guard_processes()
                save_attempted = True
                report['status'] = 'saving_single_table'
                write_report()
                if not unreal.EditorAssetLibrary.save_loaded_asset(table):
                    raise RuntimeError('单表保存失败，保留备份及当前文件哈希供恢复')
                report['saved_assets'] = [table.get_path_name()]
                report['status'] = 'saved'
            else:
                report['status'] = 'already_current'
            after = export_rows(table, output/'after.json')
            if after != expected:
                raise RuntimeError('保存后整表读回与预期不一致')
            report['after_json_sha256'] = file_hash(output/'after.json')
        report['files_after'] = disk_snapshot()
        report['dirty_after'] = dirty_packages()
        report['changed_files'] = [name for name in set(report['files_before'])|set(report['files_after'])
                                   if report['files_before'].get(name) != report['files_after'].get(name)]
        if set(report['changed_files'])-allowed_paths or report['dirty_after']:
            raise RuntimeError('发现超出单表的磁盘变化或残留脏包')
        if verify_only and report['changed_files']:
            raise RuntimeError('只读验证期间磁盘发生变化')
        report['unchanged_other_rows_and_fields'] = True
        write_report()
        marker = 'readback-latest.json' if verify_only else 'latest.json'
        (output_root/marker).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        unreal.log('COMPANION_PERSONA_ONLY '+report['status']+' '+str(output/'report.json'))
        return str(output/'report.json')
    except Exception as error:
        report['status'] = 'save_incomplete' if save_attempted else 'failed_no_assets_saved'
        report['error'] = str(error)
        # 保存前失败仅尝试恢复内存表；绝不自动覆盖磁盘或清掉其他脏包
        if not verify_only and not save_attempted and table and before is not None and dirty_packages() == [PROFILE_PATH]:
            try:
                restored = unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(before, ensure_ascii=False), row_struct)
                report['in_memory_rollback_verified'] = bool(restored and export_rows(table, output/'rollback.json') == before)
            except Exception as rollback_error:
                report['rollback_error'] = str(rollback_error)
        report['files_after'] = disk_snapshot()
        report['dirty_after'] = dirty_packages()
        write_report()
        raise



if __name__ == '__main__':
    command_line = unreal.SystemLibrary.get_command_line()
    import re
    flags = set(re.findall(r'(?<!\S)-[A-Za-z][A-Za-z0-9]*(?=\s|$)', command_line.lower()))
    persona_update = '-updatecompanionpersonaonly' in flags
    persona_verify = '-verifycompanionpersonaonly' in flags
    if persona_update or persona_verify:
        if persona_update == persona_verify or flags.intersection({'-inspectcompanionassets', '-migratecompanionencounteronly'}):
            raise RuntimeError('Persona 单表操作必须单独执行，不能混用造图或检查入口')
        update_companion_persona_only(verify_only=persona_verify)
    else:
        main('-InspectCompanionAssets' in command_line, '-MigrateCompanionEncounterOnly' in command_line)


def create_noodle_encounter_geometry(rebuild=False):
    """独立制作可开门的面馆副本，不改原店模型、关卡或角色蓝图

    在 Commandlet 中 import 本模块后显式调用，已有新资产须传 rebuild 才重建
    门叶使用厘米尺寸、正立坐标与右侧铰链；店壳仍使用原 shop1 的局部坐标
    """
    import traceback
    from collections import defaultdict

    project = Path(unreal.Paths.project_dir()).resolve()
    output = project / 'Saved/AIResearch/CompanionDoorAssets' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True)
    folder = ROOT + '/Encounter'
    paths = [folder + '/' + name for name in ('SM_NoodleShopOpen', 'SM_NoodleDoor', 'C_ErgouEjectArc')]
    source_path = '/Game/model/buildings/shop1'
    protected = [project / 'Content/model/buildings/shop1.uasset', project / 'Content/Level/TestForCharacter.umap',
                 project / 'Content/NPC/Companion/BP_Companion_Ergou.uasset']
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in protected}
    report = {'scope': 'new encounter assets and temporary Editor actors only; no PIE or map save',
              'status': 'started', 'protected_before': hashes, 'new_assets': paths}

    def vec(v):
        return [v.x, v.y, v.z]

    def serial(v):
        if isinstance(v, unreal.Object):
            return path(v)
        if isinstance(v, unreal.Vector):
            return vec(v)
        if isinstance(v, dict):
            return {str(k): serial(x) for k, x in v.items()}
        if isinstance(v, (list, tuple)):
            return [serial(x) for x in v]
        if isinstance(v, (str, int, float, bool)) or v is None:
            return v
        return str(v)

    def index_list(ids):
        return unreal.GeometryScript_List.convert_array_to_index_list(ids, unreal.GeometryScriptIndexType.TRIANGLE)

    def submesh(mesh, ids):
        result = unreal.DynamicMesh()
        unreal.GeometryScript_MeshDecomposition.get_sub_mesh_from_mesh(mesh, result, index_list(ids))
        return result

    def copy_static(mesh):
        result = unreal.DynamicMesh()
        _, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(
            mesh, result, unreal.GeometryScriptCopyMeshFromAssetOptions(),
            unreal.GeometryScriptMeshReadLOD(lod_type=unreal.GeometryScriptLODType.SOURCE_MODEL))
        if outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
            raise RuntimeError('读取模型几何失败：' + str(outcome))
        return result

    def make_mesh_asset(asset_path, dynamic):
        existing = unreal.load_asset(asset_path)
        asset = existing or unreal.EditorAssetLibrary.duplicate_asset(source_path, asset_path)
        if not asset:
            raise RuntimeError('复制店模型失败：' + asset_path)
        options = unreal.GeometryScriptCopyMeshToAssetOptions()
        options.set_editor_property('replace_materials', True)
        options.set_editor_property('new_materials', materials)
        options.set_editor_property('enable_recompute_normals', False)
        options.set_editor_property('enable_recompute_tangents', False)
        options.set_editor_property('generate_lightmap_u_vs', unreal.GeometryScriptGenerateLightmapUVOptions.DO_NOT_GENERATE_LIGHTMAP_U_VS)
        # 保留完整源模型，不能再次把 Nanite 的简化碰撞网格当成可见源模型
        nanite = asset.get_editor_property('nanite_settings')
        nanite.set_editor_property('fallback_relative_error', 0.0)
        nanite.set_editor_property('fallback_percent_triangles', 1.0)
        options.set_editor_property('apply_nanite_settings', True)
        options.set_editor_property('new_nanite_settings', nanite)
        _, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(
            dynamic, asset, options, unreal.GeometryScriptMeshWriteLOD())
        if outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
            raise RuntimeError('写入新模型失败：' + str(outcome))
        body = asset.get_editor_property('body_setup')
        body.set_editor_property('collision_trace_flag', unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
        # 强制读取渲染资源后才放置碰撞体，避免异步网格尚未就绪时误判净空
        for section in range(asset.get_num_sections(0)):
            unreal.ProceduralMeshLibrary.get_section_from_static_mesh(asset, 0, section)
        return asset

    actors = []
    try:
        if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
            raise RuntimeError('禁止在游戏世界制作初遇资产')
        for asset_path in paths:
            disk = project / ('Content' + asset_path.removeprefix('/Game') + '.uasset')
            if disk.exists():
                if not rebuild:
                    raise RuntimeError('已有资产保留人工调整，显式 rebuild 后才能覆盖：' + asset_path)
                shutil.copy2(disk, output / (disk.stem + '.before.uasset'))
        source = load(source_path)
        source.get_num_sections(0)
        materials = list(unreal.GeometryScript_AssetUtils.get_section_material_list_from_static_mesh(
            source, unreal.GeometryScriptMeshReadLOD(lod_type=unreal.GeometryScriptLODType.SOURCE_MODEL))[0])
        dynamic = copy_static(source)
        _, triangle_ids, _ = unreal.GeometryScript_MeshQueries.get_all_triangle_i_ds(dynamic)
        triangle_ids = list(unreal.GeometryScript_List.convert_index_list_to_array(triangle_ids))
        triangles = {}
        parents = {}

        def find(v):
            parents.setdefault(v, v)
            while parents[v] != v:
                parents[v] = parents[parents[v]]
                v = parents[v]
            return v

        for triangle in triangle_ids:
            valid, a, b, c = unreal.GeometryScript_MeshQueries.get_triangle_positions(dynamic, triangle)
            if not valid:
                raise RuntimeError('读取无效三角形')
            vertices = [tuple(round(x, 3) for x in vec(v)) for v in (a, b, c)]
            triangles[triangle] = vertices
            for v in vertices[1:]:
                parents[find(v)] = find(vertices[0])
        islands = defaultdict(list)
        for triangle, vertices in triangles.items():
            islands[find(vertices[0])].append(triangle)
        door_ids = []
        wall_ids = []
        wall_groups = []
        island_info = []
        for ids in islands.values():
            vertices = [v for i in ids for v in triangles[i]]
            lo = [min(v[j] for v in vertices) for j in range(3)]
            hi = [max(v[j] for v in vertices) for j in range(3)]
            is_door = all(lo[j] >= (67, 39, -156)[j] and hi[j] <= (173, 311, -123)[j] for j in range(3))
            # 限定白墙、红墙裙、木门框三个连通件，其他几何原样复制
            is_wall = any(len(ids) == count and abs(lo[0] - x) < .01
                          for count, x in ((448, -207.5), (124, -210.5), (124, 60.5)))
            if is_door:
                door_ids.extend(ids)
            elif is_wall:
                wall_ids.extend(ids)
                wall_groups.append(ids)
            if is_door or is_wall:
                island_info.append({'triangles': len(ids), 'min': lo, 'max': hi, 'door': is_door})
        report['selected_islands'] = island_info
        report['source_triangle_count'] = len(triangle_ids)
        report['source_lod'] = 'SourceModel LOD0; original Nanite fallback is not used as geometry source'
        if len(door_ids) != 1264 or len(wall_ids) != 696:
            raise RuntimeError('原模型门区发生变化，停止自动分离：' + str((len(door_ids), len(wall_ids))))
        door = submesh(dynamic, door_ids)
        remove = set(door_ids + wall_ids)
        shell = submesh(dynamic, [i for i in triangle_ids if i not in remove])
        cutter = unreal.DynamicMesh()
        unreal.GeometryScript_Primitives.append_box(
            cutter, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(location=unreal.Vector(120, 174, -90)),
            100, 280, 180, origin=unreal.GeometryScriptPrimitiveOriginMode.CENTER)
        # 三层闭合壳互相重叠，逐层切洞再拼回，避免布尔把相交的壳当成内部边界
        for group in wall_groups:
            walls = submesh(dynamic, group)
            unreal.GeometryScript_MeshBooleans.apply_mesh_boolean(
                walls, unreal.Transform(), cutter, unreal.Transform(), unreal.GeometryScriptBooleanOperation.SUBTRACT,
                unreal.GeometryScriptMeshBooleanOptions(simplify_output=False))
            unreal.GeometryScript_MeshEdits.append_mesh(shell, walls, unreal.Transform())
        report['cut_local_box'] = {'center': [120, 174, -90], 'size': [100, 280, 180]}

        world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
        actor_system = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        original = next(a for a in actor_system.get_all_level_actors() if a.get_name() == 'StaticMeshActor_7')
        component = original.get_component_by_class(unreal.StaticMeshComponent)
        if path(component.get_editor_property('static_mesh')) != source_path + '.shop1':
            raise RuntimeError('面馆场景模型已变化，停止使用旧门区配置')
        shop_transform = component.get_world_transform()
        pivot = unreal.MathLibrary.transform_location(shop_transform, unreal.Vector(172, 39.91, -127.7))
        door_transform = unreal.Transform(location=pivot, rotation=unreal.Rotator(yaw=9.91312), scale=unreal.Vector(1, 1, 1))
        unreal.GeometryScript_MeshTransforms.transform_mesh(door, shop_transform)
        unreal.GeometryScript_MeshTransforms.inverse_transform_mesh(door, door_transform)
        report['shop_transform'] = serial(shop_transform.to_dict())
        report['door_closed_transform'] = serial(door_transform.to_dict())
        shop_asset = make_mesh_asset(paths[0], shell)
        door_asset = make_mesh_asset(paths[1], door)
        report['materials'] = [path(m) for m in materials]

        def spawn(mesh, transform, label):
            actor = actor_system.spawn_actor_from_class(unreal.StaticMeshActor, transform.translation)
            actors.append(actor)
            actor.set_actor_label(label)
            part = actor.get_component_by_class(unreal.StaticMeshComponent)
            part.set_mobility(unreal.ComponentMobility.MOVABLE)
            part.set_static_mesh(mesh)
            part.set_collision_profile_name('BlockAll')
            actor.set_actor_transform(transform, False, True)
            return actor

        copy_actor = spawn(shop_asset, shop_transform, 'Companion_DoorValidationShell_TEMP')
        door_actor = spawn(door_asset, door_transform, 'Companion_DoorValidationLeaf_TEMP')
        ignored = [original] + [a for a in actor_system.get_all_level_actors() if TAG in map(str, a.tags)]
        origin = unreal.Vector(-5159.863564741846, -20679.710304395598, 2950)
        normal = unreal.Vector(-.172155, .985070, 0)
        tangent = unreal.Vector(.985070, .172155, 0)

        def point(u, d, z):
            return origin + tangent * u + normal * d + unreal.Vector(0, 0, z - 2950)

        def line(a, b):
            hit = unreal.SystemLibrary.line_trace_single(world, a, b, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                                                        True, ignored, unreal.DrawDebugTrace.NONE, True)
            return serial(hit.to_dict()) if hit else None

        def capsule(a, b):
            hit = unreal.SystemLibrary.capsule_trace_single(world, a, b, 30, 70, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                                                           True, ignored, unreal.DrawDebugTrace.NONE, True)
            return serial(hit.to_dict()) if hit else None

        # 原建筑仅作为替换前的比较对象忽略，参与所有检查的是同位置新店壳与门叶
        report['controls'] = {
            'roof': line(unreal.Vector(-5020, -21480, 7000), unreal.Vector(-5020, -21480, 2500)),
            'wall': line(point(-300, 100, 3400), point(-300, -700, 3400)),
            'closed_door': line(point(360, 100, 3200), point(360, -650, 3200)),
        }
        if not all(report['controls'].values()):
            raise RuntimeError('碰撞正控制未命中，不能把无命中当作净空')
        if not all(report['controls'][k]['hit_actor'] == path(copy_actor) for k in ('roof', 'wall')):
            raise RuntimeError('店壳碰撞正控制命中了其他物体')
        if report['controls']['closed_door']['hit_actor'] != path(door_actor):
            raise RuntimeError('闭门射线没有命中独立门叶')
        door_actor.set_actor_rotation(unreal.Rotator(yaw=99.91312), True)
        report['open_door'] = {
            'outside_in': line(point(360, -300, 3200), point(360, -650, 3200)),
            'inside_out': line(point(360, -650, 3200), point(360, -300, 3200)),
            'capsule': capsule(point(360, -550, 3190), point(360, -300, 3190)),
        }
        if any(report['open_door'].values()):
            raise RuntimeError('开门后的门洞仍被实际几何阻挡')
        # 保留原门槛木条，投掷胶囊从其上方出门；这里不是步行导航入口
        start = point(360, -550, 3190)
        landing = unreal.Vector(-5250.315522979727, -20277.443672357927, 3037.604506757589)
        curve_points = [(i / 64, 0, 0, 4 * 200 * (i / 64) * (1 - i / 64)) for i in range(65)]
        csv_path = output / 'C_ErgouEjectArc.csv'
        csv_path.write_text('\n'.join(','.join(str(v) for v in row) for row in curve_points), encoding='utf-8')
        factory = unreal.CSVImportFactory()
        settings = unreal.CSVImportSettings()
        settings.set_editor_property('import_type', unreal.CSVImportType.ECSV_CURVE_VECTOR)
        factory.set_editor_property('automated_import_settings', settings)
        task = unreal.AssetImportTask()
        task.set_editor_property('filename', str(csv_path))
        task.set_editor_property('destination_path', folder)
        task.set_editor_property('destination_name', 'C_ErgouEjectArc')
        task.set_editor_property('automated', True)
        task.set_editor_property('replace_existing', rebuild)
        task.set_editor_property('save', False)
        task.set_editor_property('factory', factory)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        curve = load(paths[2])
        samples = [start + (landing - start) * (i / 128) + curve.get_vector_value(i / 128) for i in range(129)]
        report['trajectory'] = {'start': vec(start), 'landing': vec(landing), 'curve': paths[2],
                                'curve_samples': [vec(curve.get_vector_value(t)) for t in (0, .25, .5, .75, 1)],
                                'segments': [capsule(a, b) for a, b in zip(samples, samples[1:])]}
        if any(report['trajectory']['segments']):
            raise RuntimeError('抛出轨迹仍与场景碰撞，未保存完成资产')
        report['start_clearance'] = capsule(start, start + unreal.Vector(0, 0, .1))
        if report['start_clearance']:
            raise RuntimeError('初始胶囊没有净空')
        for asset in (shop_asset, door_asset, curve):
            unreal.EditorAssetLibrary.set_metadata_tag(asset, 'DeliveryCompanionEncounterGeometry', '1')
            save(asset)
        report['status'] = 'saved_and_static_queries_passed'
        report['limitations'] = ['只验证静态射线和胶囊，不是实际 CMC 移动、动画包围体或多人演出验收',
                                 '普通门旋转没有运行时扫掠，本次旋转仅供静态开门位置检查']
        return report
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        report['traceback'] = traceback.format_exc()
        unreal.log_error(report['traceback'])
        raise
    finally:
        for actor in reversed(actors):
            unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(actor)
        report['protected_after'] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in protected}
        report['protected_hashes_unchanged'] = report['protected_before'] == report['protected_after']
        report_text = json.dumps(report, ensure_ascii=False, indent=2)
        (output / 'report.json').write_text(report_text, encoding='utf-8')
        (output.parent / 'latest.json').write_text(report_text, encoding='utf-8')
        unreal.log('COMPANION_DOOR_ASSETS_REPORT=' + str(output / 'report.json'))
