# -*- coding: utf-8 -*-
"""py add_r003_traffic_cars.py：在 MainRoad_R003 补最多5辆开局交通车。"""
import json
import math
import traceback

import unreal

from add_traffic_cars import configure_waves

ROAD_PREFIX = 'Lane_MainRoad_R003_'
COUNT = 5
TAG = 'DeliveryR003ExtraCarsBatch1'
MIN_SPACING = 1200.0
BEHIND_BOX = 250.0   # 最远 150(球半径)+83(Box 半尺寸)=233，300 在外侧接不上路
WS = unreal.SplineCoordinateSpace.WORLD
REPORT = unreal.Paths.project_saved_dir()+'add_r003_traffic_cars.json'


def gap(a,b):
    return math.hypot(a.x-b.x,a.y-b.y)


def main():
    editor=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if editor.get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate')
    if editor.get_editor_world().get_name()!='TestForCharacter':
        raise RuntimeError('请在 TestForCharacter 地图运行')
    eas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors=list(eas.get_all_level_actors())
    cars=[a for a in actors if a.get_component_by_class(unreal.DeliveryTrafficCarComponent)]
    batch=[a for a in cars if TAG in [str(t) for t in a.tags]]
    occupied=[a.get_actor_location() for a in cars]
    candidates=[]
    for a in actors:
        label=a.get_actor_label()
        if not label.startswith(ROAD_PREFIX):
            continue
        sp=a.get_component_by_class(unreal.SplineComponent)
        if not sp or sp.get_spline_length()<1500:
            continue
        p=sp.get_location_at_distance_along_spline(0,WS)
        q=sp.get_location_at_distance_along_spline(100,WS)
        dx,dy=q.x-p.x,q.y-p.y
        norm=math.hypot(dx,dy)
        if norm<0.001:
            continue
        dx,dy=dx/norm,dy/norm
        loc=unreal.Vector(p.x-dx*BEHIND_BOX,p.y-dy*BEHIND_BOX,p.z)
        candidates.append((label,loc,math.degrees(math.atan2(dy,dx))))
    if not candidates:
        raise RuntimeError('找不到 MainRoad_R003 的可用车道')
    chosen=[]
    while candidates and len(chosen)<max(0,COUNT-len(batch)):
        best=max(candidates,key=lambda row:(
            min((gap(row[1],p) for p in occupied),default=1e9),row[0]))
        spacing=min((gap(best[1],p) for p in occupied),default=1e9)
        if spacing<MIN_SPACING:
            break
        chosen.append(best)
        occupied.append(best[1])
        candidates.remove(best)

    variants=[]
    for name in ('BP_car_base','BP_car_base1','BP_car_base2','BP_car_base3',
                 'BP_car_base4','BP_car_base5'):
        cls=unreal.EditorAssetLibrary.load_blueprint_class('/Game/Vehicle/'+name)
        if cls:
            variants.append((name,cls))
    if not variants:
        raise RuntimeError('没有找到交通车型蓝图')

    rows=[]
    with unreal.ScopedEditorTransaction('MainRoad_R003补充开局交通车'):
        # 现有车辆也统一为出生后10/25秒生成后续两波。
        for car in cars:
            configure_waves(car)
        for index,(lane,loc,yaw) in enumerate(chosen):
            name,cls=variants[(len(batch)+index)%len(variants)]
            car=eas.spawn_actor_from_class(cls,loc,unreal.Rotator(0,0,yaw))
            if not car:
                raise RuntimeError('生成车辆失败: '+lane)
            car.modify(True)
            number=len(batch)+index+1
            car.set_actor_label('R003TrafficCar_%02d'%number)
            car.set_folder_path('Traffic/MainRoad_R003')
            car.tags=list(car.tags)+[TAG,'ClaudeTestCar','SourceLane:'+lane]
            scale={'BP_car_base2':0.5,'BP_car_base3':0.8}.get(name,0.7)
            car.set_actor_scale3d(unreal.Vector(scale,scale,scale))
            speed=float(700+(number-1)*150)
            car.set_editor_property('MaxSpeed',speed)
            configure_waves(car)
            assert abs(float(car.get_editor_property('MaxSpeed'))-speed)<0.1
            rows.append(dict(label=car.get_actor_label(),lane=lane,variant=name,speed=speed))

    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError('地图保存失败')
    report=dict(before=len(cars),already_added=len(batch),new=len(rows),
                after=len(cars)+len(rows),road='MainRoad_R003',
                initial='all',followup_after_start_seconds=[10,25],rows=rows)
    with open(REPORT,'w',encoding='utf-8') as output:
        json.dump(report,output,ensure_ascii=False,indent=2)
    unreal.log('R003_TRAFFIC_CARS_DONE: 原有%d，新增%d，合计%d；全部开局生成，后续10/25秒' %
               (len(cars),len(rows),report['after']))
    if len(batch)+len(rows)<COUNT:
        unreal.log_warning('MainRoad_R003 间距足够的车位不足，只补到%d辆' % (len(batch)+len(rows)))
    return report


if __name__=='__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
