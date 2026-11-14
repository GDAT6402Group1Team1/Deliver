"""把已完成的独立初遇蓝图接入正式地图，不创建或改写人物资产

-InspectCompanionEncounter：只读，记录引用、锚点、曲线和编辑器中的原生初值
-PlaceCompanionEncounter：先核验模型、蓝图、原店铺和导航，再备份并保存地图与 DA
必须显式选择一个开关；没有开关不会执行
不运行 BeginPlay、RPC、PIE、导航重建、游戏自动化或模型请求
模型和旧老板组件未准备好时停止，不能保存半套初遇场景
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil

import unreal
import setup_companion_encounter as scene_setup


MAP = '/Game/Level/TestForCharacter'
ERGOU_BP = '/Game/NPC/Companion/BP_Companion_Ergou'
TUNING = '/Game/NPC/Companion/DA_CompanionTuning'
CURVE = '/Game/NPC/Companion/Encounter/C_ErgouEjectArc'
OPEN_SHOP = '/Game/NPC/Companion/Encounter/SM_NoodleShopOpen'
ORIGINAL_SHOP = '/Game/model/buildings/shop1'
SHOP_NAME = 'StaticMeshActor_7'
SHOP_LABEL = 'ramen_shop_no_brown_diagonals'
ERGOU_TAG = 'DeliveryCompanionEncounter'
SCENE_TAG = 'DeliveryCompanionEncounterScene'
SCENE_LABEL = 'NoodleEncounter_Scene'
NAV_TAG = 'DeliveryCompanionNavigation'
SHOP_LOCATION = (-5020, -21480, 2950)
SHOP_ROTATION = (.000734, 9.913122, -89.031587)
SHOP_SCALE = (3, 3, 3)


def asset_path(value):
    return value.get_path_name() if value else None


def package_path(value):
    return asset_path(value).split('.')[0] if value else None


def load(name):
    value = unreal.load_asset(name)
    if not value:
        raise RuntimeError('缺少资产：' + name)
    return value


def vec(value):
    return [value.x, value.y, value.z]


def transform(value):
    rotation = value.rotation
    return {'location': vec(value.translation), 'rotation_quaternion': [rotation.x, rotation.y, rotation.z, rotation.w],
            'scale': vec(value.scale3d)}


def actor_snapshot(actor):
    return {'class': asset_path(actor.get_class()), 'label': actor.get_actor_label(),
            'tags': sorted(map(str, actor.tags)), 'transform': transform(actor.get_actor_transform())}


def component_snapshot(component):
    result = {'path': asset_path(component), 'relative_transform': transform(component.get_relative_transform()),
              'world_transform': transform(component.get_world_transform()),
              'visible': component.get_editor_property('visible'),
              'hidden_in_game': component.get_editor_property('hidden_in_game')}
    if isinstance(component, unreal.PrimitiveComponent):
        result.update(collision=str(component.get_collision_enabled()),
                      collision_profile=str(component.get_collision_profile_name()),
                      affects_navigation=component.get_editor_property('can_ever_affect_navigation'))
    if isinstance(component, unreal.StaticMeshComponent):
        result['mesh'] = asset_path(component.get_editor_property('static_mesh'))
    if isinstance(component, unreal.SkeletalMeshComponent):
        mesh = component.get_editor_property('skeletal_mesh_asset')
        result.update(mesh=asset_path(mesh), skeleton=asset_path(mesh.get_editor_property('skeleton')) if mesh else None,
                      animation_class=asset_path(component.get_editor_property('anim_class')),
                      animation_data=str(component.get_editor_property('animation_data')))
    if isinstance(component, unreal.MeshComponent):
        result['material_overrides'] = [asset_path(item) for item in component.get_editor_property('override_materials')]
        result['effective_materials'] = [asset_path(component.get_material(index)) for index in range(component.get_num_materials())]
    if isinstance(component, unreal.BoxComponent):
        result['box_extent'] = vec(component.get_unscaled_box_extent())
    return result


def native_state(actor, fields):
    # 只读反射字段，不调用游戏事件；未导出的字段明确记录为不可读
    result = {}
    for field in fields:
        try:
            value = actor.get_editor_property(field)
            result[field] = value if isinstance(value, (float, int, bool, str)) else str(value)
        except Exception as error:
            if field == 'encounter' and isinstance(actor, unreal.DeliveryCompanionCharacter):
                state = actor.get_encounter_state()
                result[field] = {'read_method': 'GetEncounterState BlueprintPure copy',
                    **{name: str(state.get_editor_property(name)) for name in
                       ('phase', 'revision', 'phase_started_at', 'encounter_started_at', 'fall_animation_started_at',
                        'ejection_start', 'ejection_target', 'ejection_duration', 'ejection_curve', 'failure_reason')}}
            else:
                result[field] = {'unavailable': str(error)}
    return result


def compiled(bp):
    return 'UP_TO_DATE' in str(bp.get_editor_property('status')).upper()


def same_vector(actual, expected, tolerance=.002):
    return all(abs(left - right) <= tolerance for left, right in zip(actual, expected))


def validate_shop(component):
    value = component.get_world_transform()
    rotation = value.rotation.rotator()
    if not (same_vector(vec(value.translation), SHOP_LOCATION)
            and same_vector([rotation.pitch, rotation.yaw, rotation.roll], SHOP_ROTATION)
            and same_vector(vec(value.scale3d), SHOP_SCALE, .00001)):
        raise RuntimeError('原店铺变换与已验证几何不符，保留当前地图并重新调查')
    if package_path(component.get_editor_property('static_mesh')) not in (ORIGINAL_SHOP, OPEN_SHOP):
        raise RuntimeError('原店铺网格路径已改变，不能自动替换')


def material_slots(mesh):
    return [{'slot_name': str(slot.get_editor_property('material_slot_name')),
             'imported_slot_name': str(slot.get_editor_property('imported_material_slot_name')),
             'material': asset_path(slot.get_editor_property('material_interface'))}
            for slot in mesh.get_editor_property('static_materials')]


def migrate_overrides(source_mesh, overrides, target_mesh):
    source_slots, target_slots = material_slots(source_mesh), material_slots(target_mesh)
    result = {'source_mesh': asset_path(source_mesh), 'target_mesh': asset_path(target_mesh),
              'source_slots': source_slots, 'target_slots': target_slots,
              'source_overrides': [asset_path(item) for item in overrides], 'matches': [], 'errors': []}
    if not any(overrides):
        result.update(target_overrides=[], mode='no_nonempty_override')
        return result
    mapped = [None] * len(target_slots)
    for source_index, override in enumerate(overrides):
        if not override:
            continue
        if source_index >= len(source_slots):
            result['errors'].append('原 override 超出有效材质槽：' + str(source_index))
            continue
        source = source_slots[source_index]
        if source_slots == target_slots:
            candidates = [source_index]
            reason = 'all_slot_descriptors_identical'
        else:
            candidates = [index for index, slot in enumerate(target_slots) if slot['material'] == source['material']]
            reason = 'default_material_path'
            for key in ('imported_slot_name', 'slot_name'):
                if source[key] not in ('', 'None'):
                    named = [index for index in candidates if target_slots[index][key] == source[key]]
                    if named:
                        candidates = named
                        reason += '+' + key
        if len(candidates) != 1:
            result['errors'].append('材质槽不能唯一匹配，保留原地图：' + str(source_index) + ' -> ' + str(candidates))
            continue
        destination = candidates[0]
        if mapped[destination] and mapped[destination] != asset_path(override):
            result['errors'].append('多个原 override 冲突于目标槽：' + str(destination))
            continue
        mapped[destination] = asset_path(override)
        result['matches'].append({'source_slot': source_index, 'target_slot': destination,
                                  'override': asset_path(override), 'basis': reason})
    result.update(target_overrides=mapped, mode='map_by_slot_identity')
    return result


def old_boss_components(actor, templates=False):
    if templates:
        from editor_toolset.toolsets.actor import ActorTools
        components = ActorTools.get_components(actor)
    else:
        components = actor.get_components_by_class(unreal.SceneComponent)
    return [item for item in components if 'NoodleOwner' in item.get_name()]


def snapshot_scene(actor):
    result = {'path': asset_path(actor), 'transform': transform(actor.get_actor_transform()),
              'native': native_state(actor, ['door_progress', 'door_blocked', 'door_travel_seconds', 'replicates'])}
    result['components'] = {name: component_snapshot(actor.get_editor_property(name)) for name in
                            ('door_leaf', 'noodle_owner', 'doorway_clearance', 'trigger_anchor', 'throw_start', 'throw_landing')}
    return result


def inspect_world(world, report):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    shops = [actor for actor in actors if actor.get_name() == SHOP_NAME]
    companions = [actor for actor in actors if ERGOU_TAG in map(str, actor.tags)]
    scenes = [actor for actor in actors if SCENE_TAG in map(str, actor.tags)]
    all_scenes = [actor for actor in actors if isinstance(actor, unreal.DeliveryCompanionEncounterScene)]
    report['counts'] = dict(shop=len(shops), ergou=len(companions), scene_tagged=len(scenes), scene_native=len(all_scenes))
    if len(shops) != 1 or len(companions) != 1 or len(scenes) > 1 or len(all_scenes) != len(scenes):
        raise RuntimeError('店铺、二狗或独立场景数量不符；不删除或接管未标记对象')
    shop, ergou = shops[0], companions[0]
    if shop.get_actor_label() != SHOP_LABEL or ergou.get_class() != load(ERGOU_BP).generated_class():
        raise RuntimeError('店铺标签或二狗类型不符合正式场景')
    shop_component = shop.get_component_by_class(unreal.StaticMeshComponent)
    if not shop_component:
        raise RuntimeError('原店铺缺少静态网格组件')
    report['shop'] = dict(actor=actor_snapshot(shop), component=component_snapshot(shop_component))
    report['ergou'] = dict(path=asset_path(ergou), actor=actor_snapshot(ergou),
                           mesh=component_snapshot(ergou.get_editor_property('mesh')),
                           encounter_scene=asset_path(ergou.get_editor_property('encounter_scene')),
                           tuning=asset_path(ergou.get_editor_property('companion').get_editor_property('tuning')),
                           native=native_state(ergou, ['encounter', 'encounter_task_id', 'replicates']),
                           old_boss=[component_snapshot(item) for item in old_boss_components(ergou)])
    navigation = [actor for actor in actors if isinstance(actor, unreal.NavMeshBoundsVolume)]
    report['navigation'] = {asset_path(actor): actor_snapshot(actor) for actor in navigation}
    report['companion_navigation_count'] = sum(NAV_TAG in map(str, actor.tags) for actor in navigation)
    scene = scenes[0] if scenes else None
    report['scene'] = snapshot_scene(scene) if scene else None
    return actors, shop_component, ergou, scene


def preflight(world, report):
    scene_bp, ergou_bp, tuning, curve = [load(name) for name in (scene_setup.BP_PATH, ERGOU_BP, TUNING, CURVE)]
    scene_cdo, ergou_cdo = [unreal.get_default_object(bp.generated_class()) for bp in (scene_bp, ergou_bp)]
    report['scene_blueprint'] = scene_setup.inspect(scene_bp)
    report['ergou_blueprint'] = {'path': asset_path(ergou_bp), 'status': str(ergou_bp.get_editor_property('status')),
                                'mesh': component_snapshot(ergou_cdo.get_editor_property('mesh')),
                                'old_boss_templates': [component_snapshot(item) for item in old_boss_components(ergou_cdo, True)]}
    report['tuning'] = {'path': asset_path(tuning), 'curve': asset_path(tuning.get_editor_property('ejection_curve')),
                        'duration': tuning.get_editor_property('ejection_duration')}
    report['curve'] = {'path': asset_path(curve), 'samples': {str(time): vec(curve.get_vector_value(time))
                       for time in (0, .25, .5, .75, 1)}}
    actors, shop, ergou, scene = inspect_world(world, report)
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    current_shop = shop.get_editor_property('static_mesh')
    current_overrides = list(shop.get_editor_property('override_materials'))
    material_plan = {'shell': migrate_overrides(current_shop, current_overrides, load(OPEN_SHOP)),
                     'door': migrate_overrides(current_shop, current_overrides, load(scene_setup.DOOR_PATH))}
    report['material_migration'] = material_plan
    for item in material_plan.values():
        errors.extend(item['errors'])

    try:
        validate_shop(shop)
    except RuntimeError as error:
        errors.append(str(error))
    require(compiled(scene_bp) and compiled(ergou_bp), '场景和二狗蓝图必须先编译到 UpToDate；接线脚本不编译蓝图')
    require(unreal.EditorAssetLibrary.get_metadata_tag(scene_bp, scene_setup.MARKER) == scene_setup.VERSION,
            '独立场景图版本不符，先运行对应蓝图制作流程')
    require(unreal.BlueprintEditorLibrary.get_blueprint_parent_class(scene_bp) == unreal.DeliveryCompanionEncounterScene.static_class(),
            '场景蓝图原生父类不符')
    require(report['companion_navigation_count'] == 11, '现有同伴导航体积必须保持 11 段')
    door = scene_cdo.get_editor_property('door_leaf')
    require(package_path(door.get_editor_property('static_mesh')) == scene_setup.DOOR_PATH, 'DoorLeaf 未绑定正式独立门叶')
    require(door.get_collision_enabled() == unreal.CollisionEnabled.QUERY_AND_PHYSICS, 'DoorLeaf 必须保留查询及实体碰撞')
    require(str(door.get_collision_profile_name()) in ('BlockAll', 'BlockAllDynamic'), 'DoorLeaf 碰撞配置不符')
    boss = scene_cdo.get_editor_property('noodle_owner')
    boss_mesh = boss.get_editor_property('skeletal_mesh_asset')
    ergou_mesh = ergou_cdo.get_editor_property('mesh').get_editor_property('skeletal_mesh_asset')
    resident = unreal.get_default_object(load('/Game/NPC/Residents/BP_ResidentLoud').generated_class())
    resident_mesh = resident.get_editor_property('mesh').get_editor_property('skeletal_mesh_asset')
    for role, mesh in (('二狗', ergou_mesh), ('老板', boss_mesh)):
        require(bool(mesh), role + '尚未配置独立模型')
        if mesh:
            require(mesh != resident_mesh and not package_path(mesh).startswith('/Game/Characters/NPC/SK_NpcMan'),
                    role + '仍然使用大壮或原成年男占位网格')
            require(bool(mesh.get_editor_property('skeleton')), role + '网格没有骨架')
    require(bool(ergou_mesh) and bool(boss_mesh) and ergou_mesh != boss_mesh, '二狗和老板必须分别使用不同模型')
    require(ergou.get_editor_property('mesh').get_editor_property('skeletal_mesh_asset') == ergou_mesh,
            '二狗关卡实例尚未同步新模型')
    require(ergou.get_editor_property('companion').get_editor_property('tuning') == tuning, '二狗未使用正式 CompanionTuning')
    animation = boss.get_editor_property('animation_data').get_editor_property('anim_to_play')
    require(bool(animation), '老板尚未配置对应动作')
    if animation and boss_mesh:
        require(animation.get_editor_property('skeleton') == boss_mesh.get_editor_property('skeleton'), '老板动画骨架不匹配')
    require(boss.get_collision_enabled() == unreal.CollisionEnabled.NO_COLLISION, '老板表现组件应无碰撞')
    ergou_skeleton = ergou_mesh.get_editor_property('skeleton') if ergou_mesh else None
    presentation = ergou.get_editor_property('presentation')
    for field in ('idle_animation', 'walk_animation', 'down_animation', 'get_up_animation'):
        clip = presentation.get_editor_property(field)
        require(bool(clip) and clip.get_editor_property('skeleton') == ergou_skeleton,
                '二狗身体动画尚未与当前网格兼容：' + field)
    for component in old_boss_components(ergou_cdo, True) + old_boss_components(ergou):
        require(component.get_editor_property('hidden_in_game'), '旧老板组件仍在游戏中可见：' + asset_path(component))
        require(isinstance(component, unreal.PrimitiveComponent)
                and component.get_collision_enabled() == unreal.CollisionEnabled.NO_COLLISION,
                '旧老板组件仍有碰撞：' + asset_path(component))
    for time, expected in ((0, (0, 0, 0)), (.25, (0, 0, 150)), (.5, (0, 0, 200)), (.75, (0, 0, 150)), (1, (0, 0, 0))):
        require(same_vector(vec(curve.get_vector_value(time)), expected, .01), '抛出曲线样点与静态净空验证不符')
    if scene:
        require(scene.get_class() == scene_bp.generated_class(), '已有标记场景的蓝图类型不符')
        require(same_vector(vec(scene.get_actor_location()), scene_setup.SCENE_LOCATION), '已有场景根位置不符，保留人工修改')
        rotation = scene.get_actor_rotation()
        require(same_vector([rotation.pitch, rotation.yaw, rotation.roll], [0, scene_setup.SCENE_YAW, 0]), '已有场景旋转不符')
        require(same_vector(vec(scene.get_actor_scale3d()), [1, 1, 1]), '已有场景缩放不符')
        require(scene.get_editor_property('door_leaf').get_editor_property('static_mesh') == door.get_editor_property('static_mesh'),
                '已放置门叶网格被覆盖')
        require(scene.get_editor_property('door_leaf').get_collision_enabled() == unreal.CollisionEnabled.QUERY_AND_PHYSICS,
                '已放置门叶必须保留查询及实体碰撞')
        require(str(scene.get_editor_property('door_leaf').get_collision_profile_name()) in ('BlockAll', 'BlockAllDynamic'),
                '已放置门叶碰撞配置不同')
        require(scene.get_editor_property('noodle_owner').get_editor_property('skeletal_mesh_asset') == boss_mesh,
                '已放置老板模型被覆盖')
        require(scene.get_editor_property('noodle_owner').get_collision_enabled() == unreal.CollisionEnabled.NO_COLLISION,
                '已放置老板应无碰撞')
        for field in ('door_leaf', 'doorway_clearance', 'throw_start', 'throw_landing', 'trigger_anchor'):
            require(transform(scene.get_editor_property(field).get_relative_transform())
                    == transform(scene_cdo.get_editor_property(field).get_relative_transform()),
                    '已放置场景锚点或门叶变换有覆盖：' + field)
        require(same_vector(vec(scene.get_editor_property('doorway_clearance').get_unscaled_box_extent()), scene_setup.CLEARANCE_EXTENT),
                '已放置门叶动态占用范围不符')
        existing_door_overrides = [asset_path(item) for item in scene.get_editor_property('door_leaf').get_editor_property('override_materials')]
        if any(current_overrides) and any(existing_door_overrides):
            desired = material_plan['door']['target_overrides']
            size = max(len(desired), len(existing_door_overrides))
            require(existing_door_overrides + [None]*(size-len(existing_door_overrides)) == desired + [None]*(size-len(desired)),
                    '已有场景门叶的人工材质覆盖与原店铺不同，保留原配置')
    require(ergou.get_editor_property('encounter_scene') in (None, scene), '二狗已绑定其他初遇场景，不能抢改')
    report['placement_complete'] = bool(scene) and ergou.get_editor_property('encounter_scene') == scene \
        and shop.get_editor_property('static_mesh') == load(OPEN_SHOP) \
        and tuning.get_editor_property('ejection_curve') == curve \
        and abs(tuning.get_editor_property('ejection_duration') - 1.3) < .00001
    report['preflight_errors'] = errors
    report['ready_to_place'] = not errors
    return actors, shop, ergou, scene, scene_bp, tuning, curve, material_plan


def file_hashes(files):
    return {str(item): hashlib.sha256(item.read_bytes()).hexdigest() if item.exists() else None for item in files}


def main(inspect_only=False, place=False):
    if inspect_only == place:
        raise RuntimeError('显式选择 -InspectCompanionEncounter 或 -PlaceCompanionEncounter，不能同时选择')
    project = Path(unreal.Paths.project_dir()).resolve()
    output = project / 'Saved/AIResearch/CompanionEncounterPlacement' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    relative_files = ['Level/TestForCharacter.umap', 'NPC/Companion/BP_Companion_Ergou.uasset',
                      'NPC/Companion/DA_CompanionTuning.uasset', 'NPC/Companion/Encounter/BP_NoodleEncounter.uasset',
                      'model/buildings/shop1.uasset']
    files = [project / 'Content' / name for name in relative_files]
    source_files = []
    for folder in ('Characters/NPC', 'Characters/TestDeliveryMan', 'NPC/Residents', 'UI'):
        source_files.extend((project / 'Content' / folder).rglob('*.uasset'))
    source_files.extend([project / 'Design/Art/NPC/小男孩.fbx',
                         project / 'Content/Characters/TestDeliveryMan/The Boss.fbx'])
    source_files.extend(project / 'Content/NPC/Companion/Encounter' / name
                        for name in ('SM_NoodleShopOpen.uasset', 'SM_NoodleDoor.uasset', 'C_ErgouEjectArc.uasset'))
    before_hashes = file_hashes(files)
    source_before = file_hashes(source_files)
    report = {'status': 'started', 'inspect_only': inspect_only, 'blueprint_compile_requested': False,
              'pie_started': False, 'game_rpc_called': False, 'navigation_rebuilt': False,
              'saved_packages': [], 'disk_before': before_hashes,
              'protected_sources_before': source_before,
              'limitations': ['仅静态接线与引用检查，不证明角色抛飞、门阻挡、动画、声音或多人效果已经验收']}
    mutated = False
    created = None
    rollback = None
    try:
        if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
            raise RuntimeError('游戏世界正在运行，禁止接线或只读切图')
        if unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages():
            raise RuntimeError('存在未保存地图，停止加载，先由原编辑任务保存')
        dirty = [asset_path(item) for item in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()]
        if any(name in dirty for name in (ERGOU_BP, TUNING, scene_setup.BP_PATH, ORIGINAL_SHOP)):
            raise RuntimeError('目标蓝图或配置仍有未保存编辑，停止接线')
        world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
        if not world:
            raise RuntimeError('正式地图加载失败')
        actors, shop, ergou, scene, scene_bp, tuning, curve, material_plan = preflight(world, report)
        if inspect_only:
            report['status'] = 'inspected_ready' if report['ready_to_place'] else 'inspected_incomplete'
            if scene and not report['placement_complete']:
                raise RuntimeError('已有独立场景但接线未全部完成，检查 SceneRef、店壳与 DA 曲线/时长')
            return report
        if report['preflight_errors']:
            raise RuntimeError('接线前置条件未通过：' + '；'.join(report['preflight_errors']))
        # 所有前置条件通过才创建备份和修改，不保存尚缺人物的半套地图
        for relative, original in zip(relative_files, files):
            if not original.is_file():
                raise RuntimeError('备份源文件不存在：' + str(original))
            destination = output / 'Before' / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, destination)
        report['backup_directory'] = str(output / 'Before')
        protected_actors = {asset_path(actor): actor_snapshot(actor) for actor in actors}
        (output / 'actor_state_before.json').write_text(json.dumps(protected_actors, ensure_ascii=False, indent=2), encoding='utf-8')
        report['existing_actor_count'] = len(protected_actors)
        old_mesh = shop.get_editor_property('static_mesh')
        old_overrides = list(shop.get_editor_property('override_materials'))
        old_door = scene.get_editor_property('door_leaf') if scene else None
        old_door_overrides = list(old_door.get_editor_property('override_materials')) if old_door else []
        old_scene = ergou.get_editor_property('encounter_scene')
        old_curve, old_duration = tuning.get_editor_property('ejection_curve'), tuning.get_editor_property('ejection_duration')
        rollback = (shop, old_mesh, old_overrides, old_door, old_door_overrides, ergou, old_scene, tuning, old_curve, old_duration)
        mutated = True
        if not scene:
            scene = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
                scene_bp.generated_class(), unreal.Vector(*scene_setup.SCENE_LOCATION), unreal.Rotator(yaw=scene_setup.SCENE_YAW))
            if not scene:
                raise RuntimeError('独立初遇场景放置失败')
            created = scene
            scene.set_actor_label(SCENE_LABEL)
            scene.set_editor_property('tags', [unreal.Name(SCENE_TAG)])
            scene.set_folder_path('NPC/Companion')
        shop.set_static_mesh(load(OPEN_SHOP))
        shop.set_editor_property('override_materials', [load(name) if name else None for name in material_plan['shell']['target_overrides']])
        if any(old_overrides):
            scene.get_editor_property('door_leaf').set_editor_property('override_materials',
                [load(name) if name else None for name in material_plan['door']['target_overrides']])
        ergou.set_editor_property('encounter_scene', scene)
        tuning.set_editor_property('ejection_curve', curve)
        tuning.set_editor_property('ejection_duration', 1.3)
        report['after'] = {}
        after_actors, *_ = preflight(world, report['after'])
        if report['after']['preflight_errors']:
            raise RuntimeError('接线后的静态核对失败：' + '；'.join(report['after']['preflight_errors']))
        after_snapshot = {asset_path(actor): actor_snapshot(actor) for actor in after_actors if actor != created}
        (output / 'actor_state_after.json').write_text(json.dumps(after_snapshot, ensure_ascii=False, indent=2), encoding='utf-8')
        if protected_actors != after_snapshot or report['navigation'] != report['after']['navigation']:
            raise RuntimeError('已有 Actor 或导航体积发生变化，停止保存')
        report['existing_actors_unchanged'] = True
        report['navigation_unchanged'] = True
        if not unreal.EditorAssetLibrary.save_loaded_asset(tuning, False):
            raise RuntimeError('CompanionTuning 保存失败')
        report['saved_packages'].append(TUNING)
        if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
            raise RuntimeError('正式地图保存失败；检查报告中的已保存包和备份')
        report['saved_packages'].append(MAP)
        report['status'] = 'saved_pending_independent_reload'
        report['scene_created'] = bool(created)
        return report
    except Exception as error:
        report.update(status='failed', error=str(error))
        # 尚未保存时恢复内存中的接线；已保存失败不自动覆盖磁盘，由报告指出恢复范围
        if mutated and not report['saved_packages'] and rollback:
            try:
                shop, old_mesh, old_overrides, old_door, old_door_overrides, ergou, old_scene, tuning, old_curve, old_duration = rollback
                shop.set_static_mesh(old_mesh)
                shop.set_editor_property('override_materials', old_overrides)
                if old_door:
                    old_door.set_editor_property('override_materials', old_door_overrides)
                ergou.set_editor_property('encounter_scene', old_scene)
                tuning.set_editor_property('ejection_curve', old_curve)
                tuning.set_editor_property('ejection_duration', old_duration)
                if created:
                    unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(created)
                report['in_memory_rollback'] = True
            except Exception as rollback_error:
                report['rollback_error'] = str(rollback_error)
        raise
    finally:
        after_hashes = file_hashes(files)
        source_after = file_hashes(source_files)
        allowed = {str(files[0]), str(files[2])} if not inspect_only else set()
        unexpected = [name for name in before_hashes if name not in allowed and before_hashes[name] != after_hashes[name]]
        unexpected += [name for name in source_before if source_before[name] != source_after[name]]
        report.update(disk_after=after_hashes, unexpected_changed_files=unexpected)
        report.update(protected_sources_after=source_after, protected_sources_unchanged=source_before == source_after)
        if unexpected:
            report['status'] = 'failed_protected_file_changed'
        text = json.dumps(report, ensure_ascii=False, indent=2)
        (output / 'report.json').write_text(text, encoding='utf-8')
        (output.parent / 'latest.json').write_text(text, encoding='utf-8')
        unreal.log('COMPANION_ENCOUNTER_PLACEMENT_REPORT=' + str(output / 'report.json'))
        if unexpected:
            raise RuntimeError('保护范围文件发生变化，停止后续集成：' + ', '.join(unexpected))


if __name__ == '__main__':
    command = unreal.SystemLibrary.get_command_line()
    main(bool(re.search(r'(?i)(?:^|\s)-InspectCompanionEncounter(?:\s|$)', command)),
         bool(re.search(r'(?i)(?:^|\s)-PlaceCompanionEncounter(?:\s|$)', command)))
