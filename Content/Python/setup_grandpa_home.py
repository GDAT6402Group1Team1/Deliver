"""接线爷爷搬离与现有 house2 的共用房屋标记，不生成建筑

显式 -SetupGrandpaHome 才写 BT_Grandpa、BTT_GrandpaMoveOut 和 TestForCharacter
-InspectGrandpaHome 只读检查，须在独立进程中重新加载检查保存结果
-SetupGrandpaDoorway 只保存门洞连接及重建后的当前地图导航，不改 BT 和角色
先完成 C++ 构建；本脚本不启动 PIE、测试、模型或声音，不调用任务完成/房屋认领
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil

import unreal


MAP = '/Game/Level/TestForCharacter'
TREE = '/Game/NPC/Grandpa/AI/BT_Grandpa'
TASK = '/Game/NPC/Grandpa/AI/BTT_GrandpaMoveOut'
HOME_LABEL = 'SharedPlayerHome'
SUFFIXES = {'.uasset', '.umap', '.uexp', '.ubulk', '.uptnl'}


def object_path(value):
    return value.get_path_name() if value else None


def xyz(value):
    return [value.x, value.y, value.z]


def actor_transform(actor):
    rotation = actor.get_actor_rotation()
    return [*xyz(actor.get_actor_location()), rotation.pitch, rotation.yaw, rotation.roll,
            *xyz(actor.get_actor_scale3d())]


def one(actors, predicate, name):
    found = [actor for actor in actors if predicate(actor)]
    if len(found) != 1:
        raise RuntimeError(name + ' 必须唯一，实际 ' + str(len(found)))
    return found[0]


def snapshot(project):
    paths = set()
    for folder in ('Content/NPC/Grandpa', 'Content/NPC/Companion', 'Content/Task/Definitions'):
        paths.update(path for path in (project / folder).rglob('*') if path.is_file() and path.suffix in SUFFIXES)
    paths.update(path for path in (project / 'Content/Level').glob('TestForCharacter.*') if path.suffix in SUFFIXES)
    return {path.relative_to(project).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}


def inspect(actors, house, grandpa):
    home = one(actors, lambda actor: isinstance(actor, unreal.DeliveryHome), '共用房屋标记')
    route = list(home.get_editor_property('departure_route'))
    if str(home.get_editor_property('location_id')) != 'Location_PlayerHome':
        raise RuntimeError('共用房屋地点编号不符')
    location = home.get_component_by_class(unreal.DeliveryLocationComponent)
    if not location or str(location.get_editor_property('location_id')) != 'Location_PlayerHome':
        raise RuntimeError('共用房屋地点组件未同步编号')
    if home.get_editor_property('existing_house') != house or home.get_editor_property('former_resident') != grandpa:
        raise RuntimeError('房屋标记没有关联现有 house2 和爷爷')
    if grandpa.get_editor_property('shared_home') != home:
        raise RuntimeError('爷爷 SharedHome 未关联唯一房屋')
    if len(route) < 2 or len(route) > 32:
        raise RuntimeError('搬离路线需要 2–32 个地面路点')
    if home.is_claimed():
        raise RuntimeError('编辑器默认房屋不能提前认领')
    if not unreal.DeliveryGrandpaHomeAssetTools.inspect_move_out_behavior():
        raise RuntimeError('爷爷收件分支未接可编辑搬离任务')
    anchor = home.get_editor_property('return_anchor')
    link = home.get_editor_property('doorway_link')
    if not link or link.get_editor_property('link_enabled'):
        raise RuntimeError('编辑器默认门洞连接必须存在且保持禁用')
    navigation = json.loads(unreal.DeliveryGrandpaHomeAssetTools.inspect_doorway_navigation(home))
    if navigation.get('error') or not navigation.get('restored'):
        raise RuntimeError('静态门洞导航检查失败或未恢复连接状态')
    if navigation['link_disabled']['complete'] or navigation['link_disabled']['uses_home_doorway']:
        raise RuntimeError('完整门仍有贯通路径或禁用连接被采用，需要重新核对地图')
    return {'home': object_path(home), 'house': object_path(house), 'grandpa': object_path(grandpa),
            'return_location': xyz(anchor.get_world_location()), 'route_local': [xyz(point) for point in route],
            'tree': TREE, 'task': TASK, 'claimed_default': home.is_claimed(),
            'doorway_navigation': navigation,
            'doorway_start_local': xyz(link.get_editor_property('link_relative_start')),
            'doorway_end_local': xyz(link.get_editor_property('link_relative_end')),
            'limitation': '仅编辑器资产接线；路线地板、家具、门洞和玩家阻挡须实机验证，运行时遇阻等待，不伪造抵达'}


def main():
    command = unreal.SystemLibrary.get_command_line().lower()
    doorway_setup = '-setupgrandpadoorway' in command
    setup, readonly = '-setupgrandpahome' in command or doorway_setup, '-inspectgrandpahome' in command
    if setup == readonly:
        raise RuntimeError('必须且只能指定 -SetupGrandpaHome 或 -InspectGrandpaHome')
    project = Path(unreal.Paths.project_dir()).resolve()
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = project / 'Saved/AIResearch/GrandpaHome' / stamp
    output.mkdir(parents=True)
    before = snapshot(project)
    writable = {'Content/' + name.removeprefix('/Game/') + suffix
                for name in (MAP, TREE, TASK) for suffix in SUFFIXES}
    if setup:
        for relative in writable:
            source = project / relative
            if source.exists():
                destination = output / 'backup' / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
    world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
    if not world:
        raise RuntimeError('正式地图加载失败')
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()
    house = one(actors, lambda actor: actor.get_actor_label() == 'house2', '现有房屋 house2')
    grandpa = one(actors, lambda actor: isinstance(actor, unreal.DeliveryGrandpa), '爷爷')
    grandpa_transform = actor_transform(grandpa)
    house_transform = actor_transform(house)
    if setup:
        if (house.get_actor_location() - unreal.Vector(9687, 13979, 2762)).length() > 5:
            raise RuntimeError('house2 位置已变，请重新测量锚点后更新脚本')
        homes = [actor for actor in actors if isinstance(actor, unreal.DeliveryHome)]
        conflicts = [actor for actor in actors for location in actor.get_components_by_class(unreal.DeliveryLocationComponent)
                     if str(location.get_editor_property('location_id')) == 'Location_PlayerHome'
                     and actor not in homes]
        if len(homes) > 1 or conflicts:
            raise RuntimeError('存在重复共用房屋地点，不选择或删除其中任何一份')
        if not doorway_setup and not unreal.DeliveryGrandpaHomeAssetTools.configure_move_out_behavior():
            raise RuntimeError('BT 结构与预期不符或保存失败，没有重建其他行为分支')
        if doorway_setup and len(homes) != 1:
            raise RuntimeError('门洞接线要求已有唯一共用房屋标记')
        home = homes[0] if homes else subsystem.spawn_actor_from_class(
            unreal.DeliveryHome, unreal.Vector(9812, 13360, 2010.25), unreal.Rotator())
        if not home:
            raise RuntimeError('共用房屋标记创建失败')
        home.modify()
        if not homes:
            home.set_actor_label(HOME_LABEL)
            home.set_editor_property('location_id', 'Location_PlayerHome')
            # 复用已测门洞高度：屋内 → 门外 → 院外，实际地板不符时运行时等待
            home.set_editor_property('departure_route', [unreal.Vector(0, 190, 0),
                unreal.Vector(0, -80, 0), unreal.Vector(0, -550, 0)])
        for field, expected in (('existing_house', house), ('former_resident', grandpa)):
            previous = home.get_editor_property(field)
            if previous and previous != expected:
                raise RuntimeError('已有标记 ' + field + ' 不符，保留人工配置')
            home.set_editor_property(field, expected)
        previous = grandpa.get_editor_property('shared_home')
        if previous and previous != home:
            raise RuntimeError('爷爷已有其他 SharedHome，保留人工配置')
        grandpa.modify()
        grandpa.set_editor_property('shared_home', home)
        home.get_editor_property('doorway_link').modify()
        if not unreal.DeliveryGrandpaHomeAssetTools.bake_doorway_navigation(home):
            raise RuntimeError('门洞导航构建没有完成，未保存地图')
        actors = subsystem.get_all_level_actors()
        before_save = inspect(actors, house, grandpa)
        (output / 'before_save.json').write_text(json.dumps(before_save, ensure_ascii=False, indent=2), encoding='utf-8')
        if not (before_save['doorway_navigation']['link_enabled']['complete']
                and before_save['doorway_navigation']['link_enabled']['uses_home_doorway']):
            raise RuntimeError('导航连接启用后仍无法穿过门洞，未保存地图')
        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('地图保存失败')
    result = inspect(actors, house, grandpa)
    if readonly and not (result['doorway_navigation']['link_enabled']['complete']
                         and result['doorway_navigation']['link_enabled']['uses_home_doorway']):
        raise RuntimeError('重新载入后门洞连接启用时路径不完整')
    if actor_transform(grandpa) != grandpa_transform or actor_transform(house) != house_transform:
        raise RuntimeError('爷爷或现有房屋变换意外变化')
    result.update(grandpa_transform=grandpa_transform, house_transform=house_transform)
    after = snapshot(project)
    changed = sorted(relative for relative in before.keys() | after.keys() if before.get(relative) != after.get(relative))
    unexpected = [relative for relative in changed if not setup or relative not in writable]
    result.update(mode='setup' if setup else 'inspect', changed_packages=changed, unexpected_changes=unexpected)
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    if unexpected:
        raise RuntimeError('出现范围外资产变更：' + ', '.join(unexpected))
    print('GRANDPA_HOME_OK ' + json.dumps(result, ensure_ascii=False))


main()
