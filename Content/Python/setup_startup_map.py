"""创建后台加载入口，不修改原游戏地图，也不制作开始界面布局

完整编译后执行 py setup_startup_map.py
再次执行检查入口，只补齐缺少的加载控制器，保留用户的菜单配置
"""

import unreal

STARTUP_MAP = '/Game/Level/L_MainMenu'
GAMEPLAY_MAP = '/Game/Level/TestForCharacter'


def run():
    if unreal.EditorAssetLibrary.does_asset_exist(STARTUP_MAP):
        world = unreal.EditorLoadingAndSavingUtils.load_map(STARTUP_MAP)
    else:
        source = unreal.EditorLoadingAndSavingUtils.load_map(GAMEPLAY_MAP)
        if not source:
            raise RuntimeError('Cannot load gameplay map')
        settings = source.get_world_settings()
        if settings.get_editor_property('world_partition'):
            raise RuntimeError('This setup requires a non-World-Partition gameplay map')
        game_mode = settings.get_editor_property('default_game_mode')
        if not game_mode:
            game_mode = unreal.EditorAssetLibrary.load_blueprint_class('/Game/Blueprint/GameMode/BP_DeliverGameMode')
        world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
        if not world:
            raise RuntimeError('Cannot create startup map')
        world.get_world_settings().set_editor_property('default_game_mode', game_mode)
        streaming = unreal.EditorLevelUtils.add_level_to_world(world, GAMEPLAY_MAP, unreal.LevelStreamingDynamic)
        if not streaming:
            raise RuntimeError('Cannot add gameplay streaming sublevel')
        streaming.set_editor_property('initially_loaded', False)
        streaming.set_editor_property('initially_visible', False)
        if not unreal.EditorLoadingAndSavingUtils.save_map(world, STARTUP_MAP):
            raise RuntimeError('Cannot save startup map')

    if not world:
        raise RuntimeError('Cannot load startup map')
    # 添加子关卡会改变当前编辑层，明确切回入口持久关卡再生成控制器
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not levels.set_current_level_by_name('L_MainMenu'):
        raise RuntimeError('Cannot select startup persistent level')
    persistent = levels.get_current_level()
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    flows = [a for a in actors.get_all_level_actors()
             if isinstance(a, unreal.DeliveryStartupFlow) and a.get_outer() == persistent]
    if len(flows) > 1:
        raise RuntimeError('Multiple StartupFlow actors in persistent level')
    if flows:
        unreal.log('[StartupSetup] Verified ' + flows[0].get_path_name())
        return
    flow = actors.spawn_actor_from_class(unreal.DeliveryStartupFlow, unreal.Vector(0, 0, 0))
    if not flow or flow.get_outer() != persistent:
        raise RuntimeError('StartupFlow was not created in persistent level')
    flow.set_actor_label('StartupFlow')
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, STARTUP_MAP):
        raise RuntimeError('Cannot save startup map')
    unreal.log('[StartupSetup] Saved ' + flow.get_path_name() + '; assign MenuWidgetClass')


if __name__ == '__main__':
    run()
