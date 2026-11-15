"""接线同伴专用行为树，并静态读回第一单配置

-InspectCompanionErrands：只读行为树、黑板、角色和第一单实际资产
-SetupCompanionErrands：备份四个同伴 AI 包后调用编辑器工具保存，再读回
必须显式选择一个开关；构建成功并获得本轮 Commandlet 窗口后才执行
仅写 BT_Companion / BB_Companion / BTT_CompanionFollow / BTT_CompanionErrand
不保存地图、居民、人物、UI、第一单 DA；不调用 BeginPlay、游戏 RPC、PIE、自动化或模型
保存后须用新进程 -InspectCompanionErrands 独立重载，脚本结果与进程退出码分别记录
"""
from datetime import datetime, timezone
import csv
import hashlib
import json
from pathlib import Path
import re
import shutil

import unreal


MAP = '/Game/Level/TestForCharacter'
TASK = '/Game/Task/Definitions/DA_Task_001'
ERGOU = '/Game/NPC/Companion/BP_Companion_Ergou'
AI = '/Game/NPC/Companion/AI/'
WRITABLE = tuple(AI + name for name in
                 ('BT_Companion', 'BB_Companion', 'BTT_CompanionFollow', 'BTT_CompanionErrand'))
SUCCESS = ('Saved BT_Companion, BB_Companion and persistent companion tasks',
           'BT_Companion and BB_Companion already configured')
PACKAGE_SUFFIXES = ('.uasset', '.umap', '.uexp', '.ubulk', '.uptnl')


def path(value):
    return value.get_path_name() if value else None


def package(value):
    return path(value).split('.')[0] if value else None


def serialize(value):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, unreal.Object):
        return path(value)
    if isinstance(value, unreal.Vector):
        return [value.x, value.y, value.z]
    return str(value)


def fields(value, names):
    return {name: serialize(value.get_editor_property(name)) for name in names}


def load(name):
    asset = unreal.load_asset(name)
    if not asset:
        raise RuntimeError('缺少资产：' + name)
    return asset


def actor_info(actor):
    value = actor.get_actor_transform()
    return {'path': path(actor), 'label': actor.get_actor_label(), 'class': path(actor.get_class()),
            'location': serialize(value.translation),
            'rotation': [value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w],
            'scale': serialize(value.scale3d), 'tags': sorted(map(str, actor.tags))}


def interactions(actor):
    return [{'path': path(item), **fields(item, ('interaction_key', 'interact_radius', 'hold_duration',
              'interact_enabled', 'interact_anchor', 'prompt_text'))}
            for item in actor.get_components_by_class(unreal.DeliveryInteractableComponent)]


def mesh_info(component):
    result = {'path': path(component), 'collision': str(component.get_collision_enabled()),
              'collision_profile': str(component.get_collision_profile_name())}
    if isinstance(component, unreal.SkeletalMeshComponent):
        result.update(fields(component, ('skeletal_mesh_asset', 'anim_class')))
    elif isinstance(component, unreal.StaticMeshComponent):
        result.update(fields(component, ('static_mesh',)))
    return result


def inspect_task(project, world, actors, result):
    errors, warnings = [], []
    task = load(TASK)
    names = ('task_id', 'delivery_item_id', 'pickup_location_id', 'delivery_location_id',
             'receiver_npc_id', 'time_limit_seconds', 'base_reward')
    result['definition'] = {'path': path(task), **fields(task, names)}
    result['unlock_conditions'] = []
    for condition in task.get_editor_property('unlock_conditions'):
        item = {'path': path(condition), 'class': path(condition.get_class())}
        if isinstance(condition, unreal.DeliveryTaskUnlockCondition_TimeSinceStart):
            item.update(fields(condition, ('delay_seconds',)))
        result['unlock_conditions'].append(item)
    with (project / 'Design/Tasks.csv').open(encoding='utf-8-sig', newline='') as stream:
        matching = [row for row in csv.DictReader(stream) if row['Task ID'] == 'Task_001']
    if len(matching) != 1:
        raise RuntimeError('CSV 的 Task_001 不是唯一行')
    row = matching[0]
    result['csv'] = row
    for field, column in (('task_id', 'Task ID'), ('delivery_item_id', 'Delivery Item ID'),
                          ('pickup_location_id', 'Pickup Location ID'), ('delivery_location_id', 'Delivery Location ID'),
                          ('receiver_npc_id', 'Receiver NPC ID')):
        if result['definition'][field] != row[column]:
            errors.append('任务 DA 与 CSV 不一致：' + field)
    if abs(result['definition']['time_limit_seconds'] - float(row['Time Limit'])) > .001:
        warnings.append('任务 DA 与 CSV 限时不一致，保留实际 DA 调参，不由同伴接线覆盖')
    if result['definition']['base_reward'] != int(row['Base Reward']):
        warnings.append('任务 DA 与 CSV 基础奖励不一致，保留实际 DA 调参')
    if row['Unlock Condition'] == 'time:15' and (len(result['unlock_conditions']) != 1
            or result['unlock_conditions'][0].get('delay_seconds') != 15):
        warnings.append('第一单实际解锁条件与 CSV 开局十五秒不同，保留实际 DA 配置')
    game_mode = world.get_world_settings().get_editor_property('default_game_mode')
    mode_source = 'WorldSettings.default_game_mode'
    if not game_mode:
        config = (project / 'Config/DefaultEngine.ini').read_text(encoding='utf-8-sig')
        matches = re.findall(r'^GlobalDefaultGameMode=(.+)$', config, flags=re.MULTILINE)
        if len(matches) != 1:
            raise RuntimeError('地图没有 GameMode 覆盖，项目默认 GameMode 不能唯一解析')
        game_mode = unreal.load_class(None, matches[0].strip())
        mode_source = 'Config/DefaultEngine.ini GlobalDefaultGameMode'
    if not game_mode:
        raise RuntimeError('实际 GameMode 类加载失败')
    state_class = unreal.get_default_object(game_mode).get_editor_property('game_state_class')
    if not state_class:
        raise RuntimeError('实际 GameMode 没有 GameStateClass')
    manager = unreal.get_default_object(state_class).get_component_by_class(unreal.DeliveryTaskManagerComponent)
    if not manager:
        raise RuntimeError('实际 GameState CDO 缺少 TaskManager')
    definitions = list(manager.get_all_task_definitions())
    result['game_state'] = {'game_mode': path(game_mode), 'mode_source': mode_source,
                            'class': path(state_class), 'manager': path(manager),
                            'task_definitions': [path(item) for item in definitions]}
    if sum(item == task for item in definitions) != 1:
        errors.append('实际 GameState 任务清单未唯一包含 DA_Task_001')

    locations, pickups, targets = [], [], []
    pickup_id, delivery_id = row['Pickup Location ID'], row['Delivery Location ID']
    for actor in actors:
        actor_locations = actor.get_components_by_class(unreal.DeliveryLocationComponent)
        for location in actor_locations:
            location_id = str(location.get_editor_property('location_id'))
            if location_id in (pickup_id, delivery_id):
                locations.append({'actor': actor_info(actor), 'path': path(location), 'id': location_id,
                                  'location': serialize(location.get_world_location())})
        for pickup in actor.get_components_by_class(unreal.DeliveryPickupPointComponent):
            # GetLocationId 只读取同 Actor 的地点组件，不触发注册或发放物品
            if str(pickup.get_location_id()) == pickup_id:
                pickups.append({'actor': actor_info(actor), 'path': path(pickup),
                    **fields(pickup, ('package_class', 'spawn_offset', 'interact_radius')),
                    'interactions': interactions(actor)})
        for target in actor.get_components_by_class(unreal.DeliveryTargetComponent):
            if target.get_editor_property('expected_task') == task:
                targets.append({'actor': actor_info(actor), 'path': path(target),
                    **fields(target, ('expected_task', 'delivery_radius', 'interact_radius')),
                    'interactions': interactions(actor)})
    result.update(locations=locations, pickups=pickups, targets=targets)
    for location_id in (pickup_id, delivery_id):
        if sum(item['id'] == location_id for item in locations) != 1:
            errors.append('第一单地点必须唯一：' + location_id)
    if len(pickups) != 1:
        errors.append('第一单面馆取货组件必须唯一')
    elif not pickups[0]['package_class']:
        warnings.append('面馆 PackageClass 为空；原生代码使用 DeliveryTestPackage 兜底，此处不改资产')
    if pickups and pickups[0]['interactions'] and not pickups[0]['interactions'][0]['interact_enabled']:
        errors.append('面馆首个取货交互被禁用，BeginPlay 配置不会自动重新开启')
    grandpas = [actor for actor in actors if actor.get_actor_label() == row['Receiver NPC ID']]
    if len(grandpas) != 1 or not isinstance(grandpas[0], unreal.DeliveryGrandpa):
        errors.append('第一单爷爷标签或类型不符')
    else:
        grandpa = grandpas[0]
        door = grandpa.get_editor_property('front_door')
        result['grandpa'] = {'actor': actor_info(grandpa), **fields(grandpa, ('front_door', 'behavior')),
                             'body': mesh_info(grandpa.get_editor_property('body'))}
        if len(targets) != 1 or targets[0]['actor']['path'] != path(grandpa):
            errors.append('第一单交付点不唯一或仍有房屋旧交付入口')
        if not door or not isinstance(door, unreal.DeliveryBreakableDoor):
            errors.append('爷爷 FrontDoor 没有绑定真实破坏门')
        else:
            result['grandpa_door'] = {'actor': actor_info(door), **fields(door, ('hits_to_break', 'intact_door', 'fragments')),
                'meshes': [mesh_info(item) for item in door.get_components_by_class(unreal.StaticMeshComponent)]}
    axes = [actor for actor in actors if actor.get_actor_label() == 'OldManHouse_Axe']
    result['axes'] = [{'actor': actor_info(actor), **fields(actor, ('uses_axe_swing',)),
                      'meshes': [mesh_info(item) for item in actor.get_components_by_class(unreal.StaticMeshComponent)]}
                     for actor in axes]
    if len(axes) != 1 or not result['axes'][0]['uses_axe_swing']:
        errors.append('第一单正式斧头缺少、重复或未启用挥斧')
    houses = [actor for actor in actors if actor.get_actor_label() == 'house2']
    result['house_interactions'] = [{'actor': actor_info(actor), 'interactions': interactions(actor)} for actor in houses]
    if any(item['interact_enabled'] for house in result['house_interactions'] for item in house['interactions']):
        errors.append('原爷爷房屋仍有启用的旧交互，需人工核对')
    result.update(errors=errors, warnings=warnings,
                  limitation='只读编辑器配置；BeginPlay 创建的敲门组件、交互委托及真实按键未执行')
    return result


def inspect_companion(actors):
    bp = load(ERGOU)
    cdo = unreal.get_default_object(bp.generated_class())
    placed = [actor for actor in actors if isinstance(actor, unreal.DeliveryCompanionCharacter)]
    result = {'blueprint': path(bp), 'blueprint_status': str(bp.get_editor_property('status')),
              'class_defaults': fields(cdo, ('behavior_tree', 'ai_controller_class', 'auto_possess_ai', 'encounter_task_id')),
              'instances': [], 'errors': []}
    for actor in placed:
        companion = actor.get_editor_property('companion')
        tuning = companion.get_editor_property('tuning')
        scene = actor.get_editor_property('encounter_scene')
        item = {'actor': actor_info(actor), **fields(actor, ('behavior_tree', 'ai_controller_class', 'auto_possess_ai',
                    'encounter_task_id', 'encounter_scene')),
                'mesh': mesh_info(actor.get_editor_property('mesh')), 'tuning': path(tuning),
                'package_anchor': str(actor.get_editor_property('package_anchor').get_relative_transform()),
                'help': fields(actor.get_editor_property('help_interactable'),
                    ('interaction_key', 'hold_duration', 'interact_radius', 'prompt_text'))}
        if tuning:
            item['tuning_values'] = fields(tuning, ('errand_timeout_seconds', 'errand_interaction_timeout_seconds',
                'ejection_curve', 'ejection_duration', 'help_hold_seconds', 'help_recovery_seconds', 'encounter_radius'))
        if scene:
            item['scene_boss'] = mesh_info(scene.get_editor_property('noodle_owner'))
            item['scene_door'] = mesh_info(scene.get_editor_property('door_leaf'))
            item['trigger_location'] = serialize(scene.get_trigger_world_location())
        item['old_boss_components'] = [path(component) for component in actor.get_components_by_class(unreal.SceneComponent)
                                       if 'NoodleOwner' in component.get_name()]
        result['instances'].append(item)
        if package(actor.get_editor_property('behavior_tree')) != AI + 'BT_Companion':
            result['errors'].append('地图二狗 BehaviorTree 引用错误')
        if not tuning or not scene:
            result['errors'].append('地图二狗缺少 Tuning 或 EncounterScene')
        if item['old_boss_components']:
            result['errors'].append('地图二狗仍有旧老板组件')
    if len(placed) != 1:
        result['errors'].append('正式地图二狗实例数量不是一')
    if package(cdo.get_editor_property('behavior_tree')) != AI + 'BT_Companion':
        result['errors'].append('二狗 CDO BehaviorTree 引用错误')
    result['navigation'] = [actor_info(actor) for actor in actors if isinstance(actor, unreal.NavMeshBoundsVolume)]
    result['companion_navigation_count'] = sum('DeliveryCompanionNavigation' in item['tags'] for item in result['navigation'])
    if result['companion_navigation_count'] != 11:
        result['errors'].append('既有同伴导航体积数量不是十一')
    result['player_starts'] = [actor_info(actor) for actor in actors if isinstance(actor, unreal.PlayerStart)]
    return result


def describe_tree():
    describe = getattr(unreal.DeliveryNpcToolset, 'describe_companion_behavior_asset', None)
    if not describe:
        raise RuntimeError('当前 DLL 没有 DescribeCompanionBehaviorAsset，请先完成本轮完整构建')
    value = json.loads(describe())
    if value.get('schema_version') != 1 or not isinstance(value.get('root'), dict):
        raise RuntimeError('行为树只读返回结构不符：' + json.dumps(value, ensure_ascii=False))
    return value


def validate_tree(tree):
    errors = list(tree.get('validation_errors', []))
    blackboard_path = tree.get('blackboard_path') or ''
    if blackboard_path.split('.')[0] != AI + 'BB_Companion':
        errors.append('行为树没有引用独立 BB_Companion')
    children = tree['root'].get('children', [])
    expected_keys = ['Unavailable', 'Listening', 'HasApprovedAction', 'HasCompanionErrand', None]
    search_key = 'HasNearbyItemSearch'
    blackboard_keys = tree.get('blackboard', {}).get('keys', [])
    search_installed = len(children) == 6
    if search_installed:
        expected_keys.insert(3, search_key)
    if len(children) != len(expected_keys):
        errors.append('根 Selector 需要五个委托基础分支，接入找物后需要六个')
        return errors
    if not tree['root'].get('class', '').endswith('BTComposite_Selector'):
        errors.append('行为树根不是 Selector')
    for index, (child, key) in enumerate(zip(children, expected_keys)):
        decorators = child.get('decorators', [])
        if key is None:
            if decorators:
                errors.append('默认 Follow 分支不能带装饰器')
        elif len(decorators) != 1 or decorators[0].get('key') != key:
            errors.append('分支装饰器 key 不符：' + str(index))
        elif decorators[0].get('flow_abort_mode_name') != 'Both' or decorators[0].get('basic_operation_name') != 'Set':
            errors.append('分支必须按 Set/Both 中止：' + key)
        node = child.get('node', {})
        if key in ('HasCompanionErrand', None):
            suffix = 'BTT_CompanionErrand_C' if key else 'BTT_CompanionFollow_C'
            if key is None and len(node.get('children', [])) == 1:
                if node['children'][0].get('decorators'):
                    errors.append('默认 Follow 叶子不应有装饰器')
                node = node['children'][0].get('node', {})
            if not node.get('class', '').endswith(suffix):
                errors.append('持续任务叶子类型不符：' + suffix)
            if key == 'HasCompanionErrand' and node.get('search_stage_only') is True:
                errors.append('持续委托叶子必须允许推进非找物阶段')
        if key == search_key:
            subtree = node.get('subtree', {})
            if (not node.get('class', '').endswith('BTTask_RunBehavior') or
                    subtree.get('path') != '/Game/NPC/Shared/AI/BT_SearchNearbyItem.BT_SearchNearbyItem' or
                    subtree.get('blackboard_path') != blackboard_path):
                errors.append('找物分支必须运行正式子树并共享 BB_Companion')
            root = subtree.get('root', {})
            tasks = root.get('children', [])
            if (not root.get('class', '').endswith('BTComposite_Sequence') or len(tasks) != 1 or
                    tasks[0].get('node', {}).get('search_stage_only') is not True):
                errors.append('找物子树需要仅执行搜索阶段的委托任务')
    for required in ('HasCompanionErrand', search_key) if search_installed else ('HasCompanionErrand',):
        keys = [key for key in blackboard_keys if key.get('name') == required]
        if len(keys) != 1 or not keys[0].get('type', '').endswith('BlackboardKeyType_Bool') or keys[0].get('instance_synced'):
            errors.append(required + ' 必须是非实例同步 Bool')
    if not search_installed and any(key.get('name') == search_key for key in blackboard_keys):
        errors.append('黑板已有找物键，正式找物分支尚未接好')
    for node in tree.get('graph', {}).get('nodes', []):
        if 'blackboard_path' in node and (node['blackboard_path'] or '').split('.')[0] != AI + 'BB_Companion':
            errors.append('可编辑图根仍引用旧黑板')
    return errors


def compare_graph(before, after):
    def nodes_by_guid(tree):
        found = {}
        def visit(node):
            if node.get('already_described'):
                return
            found[node['guid']] = node
            for child in node.get('sub_nodes', []):
                visit(child)
        for node in tree.get('graph', {}).get('nodes', []):
            visit(node)
        return found
    before_nodes, after_nodes = nodes_by_guid(before), nodes_by_guid(after)
    errors = []
    for old in before_nodes.values():
        new = after_nodes.get(old['guid'])
        if not new:
            errors.append('旧编辑图节点丢失：' + old['guid'])
            continue
        for field in ('x', 'y', 'instance_path', 'comment', 'decorator_paths', 'service_paths'):
            if old.get(field) != new.get(field):
                errors.append('旧编辑图节点字段变化：' + old['guid'] + '/' + field)
    return errors


def package_base(project, name):
    return project / 'Content' / name.removeprefix('/Game/')


def hash_file(filename):
    digest = hashlib.sha256()
    with filename.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def protected_files(project):
    files = set()
    # 保护原地图、整个居民/UI/第一单配置与现有人物；脚本只开放四个同伴行为包
    for folder in ('NPC', 'UI', 'Task', 'Blueprint', 'Characters/NPC', 'Characters/TestDeliveryMan'):
        files.update(item for item in (project / 'Content' / folder).rglob('*')
                     if item.is_file() and item.suffix.lower() in PACKAGE_SUFFIXES)
    files.update(project / name for name in ('Content/Level/TestForCharacter.umap', 'Content/model/buildings/shop1.uasset',
        'Design/Art/NPC/小男孩.fbx', 'Content/Characters/TestDeliveryMan/The Boss.fbx', 'Config/DefaultEngine.ini'))
    files.update((project / 'Content/Level/TestForCharacter').with_suffix(suffix) for suffix in PACKAGE_SUFFIXES)
    allowed = {package_base(project, name).with_suffix(suffix) for name in WRITABLE for suffix in PACKAGE_SUFFIXES}
    return {str(item): hash_file(item) for item in sorted(files - allowed) if item.exists()}


def writable_hashes(project):
    return {str(item): hash_file(item) if item.exists() else None
            for name in WRITABLE for suffix in PACKAGE_SUFFIXES
            for item in (package_base(project, name).with_suffix(suffix),)}


def main(inspect_only=False, setup=False):
    if inspect_only == setup:
        raise RuntimeError('显式选择 -InspectCompanionErrands 或 -SetupCompanionErrands，不能同时选择')
    project = Path(unreal.Paths.project_dir()).resolve()
    output = project / 'Saved/AIResearch/CompanionErrandAssets' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'started', 'inspect_only': inspect_only, 'writable_packages': list(WRITABLE),
              'pie_started': False, 'game_rpc_called': False, 'automation_run': False, 'model_requested': False,
              'map_saved': False, 'tool_called': False, 'process_exit_code': None,
              'limitations': ['仅静态配置与图结构，不证明真实取件、帮送、交还、取消或多人玩法通过']}
    protected_before = protected_files(project)
    writable_before = writable_hashes(project)
    report.update(protected_before=protected_before, writable_before=writable_before)
    try:
        if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
            raise RuntimeError('PIE运行中禁止接线或检查')
        dirty = list(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages())
        dirty += list(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages())
        report['initial_dirty_packages'] = [path(item) for item in dirty]
        if dirty:
            raise RuntimeError('已有未保存的编辑器包，保留现场并停止本次操作')
        report['tree_before'] = describe_tree()
        world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
        if not world:
            raise RuntimeError('正式地图加载失败')
        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
        report['first_order'] = {}
        inspect_task(project, world, actors, report['first_order'])
        report['companion'] = inspect_companion(actors)
        report['static_errors'] = report['first_order']['errors'] + report['companion']['errors']
        report['candidate_configuration'] = {
            'basis': '源码 CanExecute/ExtendDecisionRequest；未调用运行时查询',
            'requires_additional_blueprint_allowlist': False,
            'runtime_candidates': ['FetchPackage', 'TakePackage', 'DeliverPackage', 'ReturnPackage'],
            'guards': 'native 主人身份、任务、持件、委托状态与能力版本 3；编辑器实例未认主不应产生运行候选'}
        if report['static_errors']:
            raise RuntimeError('第一单或二狗静态配置不符：' + '；'.join(report['static_errors']))
        if setup:
            backup = output / 'Before'
            for name, digest in writable_before.items():
                if digest:
                    source = Path(name)
                    destination = backup / source.relative_to(project)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)
                    if hash_file(destination) != digest:
                        raise RuntimeError('行为资产备份哈希不符：' + name)
            report['backup_directory'] = str(backup)
            report['tool_called'] = True
            report['tool_result'] = unreal.DeliveryNpcToolset.create_companion_behavior_asset()
            if report['tool_result'] not in SUCCESS:
                raise RuntimeError('行为树工具未报告成功：' + report['tool_result'])
        report['tree_after'] = describe_tree()
        report['behavior_errors'] = validate_tree(report['tree_after'])
        report['graph_preservation_errors'] = compare_graph(report['tree_before'], report['tree_after'])
        if report['behavior_errors'] or report['graph_preservation_errors']:
            raise RuntimeError('行为树读回不符：' + '；'.join(report['behavior_errors'] + report['graph_preservation_errors']))
        report['status'] = 'inspected_ready' if inspect_only else 'saved_pending_independent_reload'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        protected_after = protected_files(project)
        writable_after = writable_hashes(project)
        unexpected = sorted(name for name in protected_before.keys() | protected_after.keys()
                            if protected_before.get(name) != protected_after.get(name))
        if inspect_only:
            unexpected += [name for name in writable_before if writable_before[name] != writable_after[name]]
        report.update(protected_after=protected_after, protected_unchanged=not unexpected,
                      writable_after=writable_after, unexpected_changed_files=unexpected,
                      changed_behavior_files=[name for name in writable_before if writable_before[name] != writable_after[name]])
        if unexpected:
            report['status'] = 'failed_protected_file_changed'
        text = json.dumps(report, ensure_ascii=False, indent=2)
        (output / 'report.json').write_text(text, encoding='utf-8')
        (output.parent / 'latest.json').write_text(text, encoding='utf-8')
        unreal.log('COMPANION_ERRAND_ASSET_REPORT=' + str(output / 'report.json'))
        if unexpected:
            raise RuntimeError('保护范围发生磁盘变化，停止后续工作：' + ', '.join(unexpected))
    return report


if __name__ == '__main__':
    command = unreal.SystemLibrary.get_command_line()
    main(bool(re.search(r'(?i)(?:^|\s)-InspectCompanionErrands(?:\s|$)', command)),
         bool(re.search(r'(?i)(?:^|\s)-SetupCompanionErrands(?:\s|$)', command)))
