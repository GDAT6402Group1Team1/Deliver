# -*- coding: utf-8 -*-
"""py add_traffic_cars.py：累计补30辆，并统一全部关卡交通车的三波配置。"""
import json
import math
import random
import traceback
import unreal

ADD_COUNT = 30  # 本批累计数量；重复运行只补缺失的，不每次再加30辆
TAG = 'DeliveryExtraTrafficCarsBatch1'
MIN_SPACING = 1200.0
BEHIND_BOX = 250.0   # 最远 150(球半径)+83(Box 半尺寸)=233，300 在外侧接不上路
WS = unreal.SplineCoordinateSpace.WORLD
SCALES = {'BP_car_base2':0.5,'BP_car_base3':0.8}


def configure_waves(car):
    traffic=car.get_component_by_class(unreal.DeliveryTrafficCarComponent)
    if not traffic:raise RuntimeError(car.get_actor_label()+': 缺少交通组件')
    car.modify(True)
    traffic.modify(True)
    traffic.set_editor_property('spawn_extra_waves',True)
    traffic.set_editor_property('second_wave_delay',10.0)
    traffic.set_editor_property('third_wave_delay',15.0)
    assert traffic.get_editor_property('spawn_extra_waves'),car.get_actor_label()
    assert abs(traffic.get_editor_property('second_wave_delay')-10.0)<0.001
    assert abs(traffic.get_editor_property('third_wave_delay')-15.0)<0.001


def distance(a,b):
    return math.sqrt((a.x-b.x)**2+(a.y-b.y)**2+(a.z-b.z)**2)


def main(dry_run=False,save=True):
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate，再加车')
    world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world.get_name()!='TestForCharacter':
        raise RuntimeError('请在 TestForCharacter 地图运行')
    eas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors=list(eas.get_all_level_actors())
    cars=[a for a in actors if a.get_class().get_name().startswith('BP_car_base')]
    added=[a for a in cars if TAG in [str(t) for t in a.tags]]
    occupied=[a.get_actor_location() for a in cars]
    candidates=[]
    for a in actors:
        if not a.get_actor_label().startswith('Lane_'): continue
        sp=a.get_component_by_class(unreal.SplineComponent)
        if not sp or sp.get_spline_length()<1500: continue
        p=sp.get_location_at_distance_along_spline(0,WS)
        q=sp.get_location_at_distance_along_spline(100,WS)
        dx,dy=q.x-p.x,q.y-p.y
        norm=math.hypot(dx,dy)
        if norm<0.001: continue
        dx,dy=dx/norm,dy/norm
        loc=unreal.Vector(p.x-dx*BEHIND_BOX,p.y-dy*BEHIND_BOX,p.z)
        candidates.append((a.get_actor_label(),loc,math.degrees(math.atan2(dy,dx))))
    remaining=max(0,ADD_COUNT-len(added))
    chosen=[]
    # 每次取距现有车最远的空位，新增车辆立即参与间距判断。
    while candidates and len(chosen)<remaining:
        scores=[min((distance(row[1],p) for p in occupied),default=1e9) for row in candidates]
        index=max(range(len(scores)),key=lambda i:scores[i])
        if scores[index]<MIN_SPACING: break
        row=candidates.pop(index)
        chosen.append(row);occupied.append(row[1])
    variants=[]
    for name in ('BP_car_base','BP_car_base1','BP_car_base2','BP_car_base3','BP_car_base4','BP_car_base5'):
        cls=unreal.EditorAssetLibrary.load_blueprint_class('/Game/Vehicle/'+name)
        if cls:variants.append((name,cls))
    if not variants:raise RuntimeError('没有找到车型蓝图')
    rng=random.Random(20261022)
    rng.shuffle(variants)
    labels={a.get_actor_label() for a in actors}
    report=dict(dry_run=dry_run,before=len(cars),batch_before=len(added),
                planned=len(chosen),target_batch=ADD_COUNT,existing_updated=len(cars),
                extra_waves=True,wave_times_seconds=[0,10,25],rows=[])
    with unreal.ScopedEditorTransaction('统一交通车分波设置'):
        if not dry_run:
            for car in cars:
                configure_waves(car)
        for index,(lane,loc,yaw) in enumerate(chosen):
            name,cls=variants[index%len(variants)]
            speed=float(600+(index%10)*100)
            row=dict(lane=lane,variant=name,speed=speed,location=[loc.x,loc.y,loc.z])
            report['rows'].append(row)
            if dry_run:continue
            car=eas.spawn_actor_from_class(cls,loc,unreal.Rotator(0,0,yaw))
            if not car:raise RuntimeError('生成车辆失败: '+lane)
            car.modify(True)
            number=len(added)+index+1
            label='ExtraTrafficCar_%03d'%number
            while label in labels:
                number+=1;label='ExtraTrafficCar_%03d'%number
            labels.add(label)
            car.set_actor_label(label)
            car.set_folder_path('Traffic/ExtraCars')
            car.tags=list(car.tags)+[TAG,'ClaudeTestCar','SourceLane:'+lane]
            scale=SCALES.get(name,0.7)
            car.set_actor_scale3d(unreal.Vector(scale,scale,scale))
            car.set_editor_property('MaxSpeed',speed)
            configure_waves(car)
            assert abs(float(car.get_editor_property('MaxSpeed'))-speed)<0.1,label
            row['label']=label
    if not dry_run and (chosen or cars) and save:
        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('地图保存失败')
    report['after']=len(cars)+len(chosen)
    with open(unreal.Paths.project_saved_dir()+'add_traffic_cars.json','w',encoding='utf-8') as f:
        json.dump(report,f,ensure_ascii=False,indent=2)
    unreal.log('ADD_TRAFFIC_CARS: 原有%d，补充%d，合计%d；已统一现有%d辆的分波配置；分波时间0/10/25秒' % (len(cars),len(chosen),report['after'],len(cars)))
    return report


if __name__=='__main__':
    try:main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
