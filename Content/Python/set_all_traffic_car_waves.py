# -*- coding: utf-8 -*-
"""py set_all_traffic_car_waves.py：统一当前关卡全部交通车的三个生成波次。"""
import json
import traceback

import unreal

from add_traffic_cars import configure_waves


def main():
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if editor.get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate')
    if editor.get_editor_world().get_name() != 'TestForCharacter':
        raise RuntimeError('请在 TestForCharacter 地图运行')

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    cars = [actor for actor in actors
            if actor.get_component_by_class(unreal.DeliveryTrafficCarComponent)]
    if not cars:
        raise RuntimeError('当前关卡没有找到交通车')

    with unreal.ScopedEditorTransaction('全部交通车统一为三波生成'):
        for car in cars:
            configure_waves(car)

    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError('地图保存失败')

    report = {'cars': len(cars), 'wave_times_seconds': [0, 10, 25],
              'labels': sorted(car.get_actor_label() for car in cars)}
    with open(unreal.Paths.project_saved_dir() + 'all_traffic_car_waves.json',
              'w', encoding='utf-8') as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
    unreal.log('ALL_TRAFFIC_CAR_WAVES_DONE: %d辆，生成时刻0/10/25秒' % len(cars))
    return report


if __name__ == '__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
