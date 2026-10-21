# -*- coding: utf-8 -*-
"""py fix_traffic_light_directions.py：修正灯面90度偏差，对向同起始色同周期，整组重合。"""
import datetime
import json
import math
import traceback
import importlib
import unreal
from gen_traffic_lights import child_directions,read_prop,INTERSECTION_PREFIX,TAG,LIGHT_BP,GREEN_START_BP
# 编辑器 Python 会缓存模块；重跑脚本时必须重新读取最新的起始分组设置。
import traffic_light_directions
importlib.reload(traffic_light_directions)
from traffic_light_directions import normalized_directions,MODEL_YAW_OFFSET,GREEN_FIRST_GROUP

ROOT=unreal.Paths.project_saved_dir()


def enum_int(v):
    return int(getattr(v,'value',v))


def main(dry_run=False,save=True):
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate')
    eas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors=list(eas.get_all_level_actors())
    inters=[a for a in actors if a.get_class().get_name().startswith(INTERSECTION_PREFIX)]
    classes={False:unreal.EditorAssetLibrary.load_blueprint_class(LIGHT_BP),
             True:unreal.EditorAssetLibrary.load_blueprint_class(GREEN_START_BP)}
    if not all(classes.values()): raise RuntimeError('缺少红/绿起始灯蓝图')
    for green,cls in classes.items():
        if enum_int(unreal.get_default_object(cls).get_editor_property('Start Color'))!=(0 if green else 2):
            raise RuntimeError('蓝图起始色不符合定义: '+cls.get_name())
    plans=[]; backup=[]; warnings=[]
    for inter in inters:
        lights=[]
        for a in actors:
            for key in range(4):
                if a.get_actor_label()=='TrafficLight_%s_%d'%(inter.get_actor_label()[-6:],key):
                    lights.append((a,key))
        if not lights: continue
        dirs=child_directions(inter,actors)
        corrected,notes=normalized_directions(dirs)
        warnings.extend(inter.get_actor_label()+': '+s for s in notes)
        positions=[a.get_actor_location() for a,key in lights]
        common=unreal.Vector(sum(p.x for p in positions)/len(positions),
                             sum(p.y for p in positions)/len(positions),sum(p.z for p in positions)/len(positions))
        cycle=float(read_prop(inter,'LightDuration'))
        if cycle<=1: raise RuntimeError(inter.get_actor_label()+': 路口周期无效')
        for a,key in lights:
            if key not in corrected:
                warnings.append(a.get_actor_label()+': 未找到对应来车方向，跳过')
                continue
            group=corrected[key]['group']
            green=group==GREEN_FIRST_GROUP
            yaw=corrected[key]['travel_yaw']+180+MODEL_YAW_OFFSET
            p=a.get_actor_location();r=a.get_actor_rotation();s=a.get_actor_scale3d()
            backup.append(dict(label=a.get_actor_label(),cls=a.get_class().get_path_name(),
                position=[p.x,p.y,p.z],rotation=[r.roll,r.pitch,r.yaw],scale=[s.x,s.y,s.z],
                folder=str(a.get_folder_path()),tags=[str(t) for t in a.tags],
                values={n:str(read_prop(a,n)) for n in ('Start Color','Green Light Duration','Yellow Light Duration','Red Light Duration')}))
            plans.append((a,key,inter,classes[green],common,yaw,group,cycle))
    report=dict(dry_run=dry_run,lights=len(plans),warnings=warnings,rows=[])
    if not dry_run:
        path=ROOT+'traffic_light_direction_backup_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json'
        with open(path,'w',encoding='utf-8') as f:json.dump(backup,f,ensure_ascii=False,indent=2)
        report['backup']=path
    with unreal.ScopedEditorTransaction('修正红绿灯朝向与对向相位'):
        for a,key,inter,cls,common,yaw,group,cycle in plans:
            label=a.get_actor_label()
            replaced=a.get_class()!=cls
            row=dict(label=label,intersection=inter.get_actor_label(),key=key,group=group,
                     before_yaw=a.get_actor_rotation().yaw,after_yaw=yaw,start='green' if group==GREEN_FIRST_GROUP else 'red',replace_class=replaced)
            report['rows'].append(row)
            if dry_run: continue
            a.modify(True)
            if replaced:
                old=a
                a=eas.spawn_actor_from_class(cls,common,unreal.Rotator(0,0,yaw))
                if not a: raise RuntimeError('生成修正灯失败: '+label)
                a.set_actor_label(label)
                a.set_folder_path(old.get_folder_path())
                a.set_actor_scale3d(old.get_actor_scale3d())
                a.tags=[str(t) for t in old.tags]
                if not eas.destroy_actor(old): raise RuntimeError('替换旧灯失败: '+label)
            tags=[str(t) for t in a.tags if not str(t).startswith(TAG+':')]
            if TAG not in tags:tags.append(TAG)
            a.tags=tags+[TAG+':'+str(group)]
            for name,val in (('Green Light Duration',cycle-1),('Yellow Light Duration',1.0),('Red Light Duration',cycle)):
                if abs(float(a.get_editor_property(name))-val)>0.001:
                    a.set_editor_property(name,val)
                assert abs(float(a.get_editor_property(name))-val)<0.001,label
            a.set_actor_rotation(unreal.Rotator(0,0,yaw),True)
            a.set_actor_location(common,False,True)
            assert enum_int(a.get_editor_property('Start Color'))==(0 if group==GREEN_FIRST_GROUP else 2),label
            spots=list(a.get_components_by_class(unreal.SpotLightComponent))
            assert spots,label+': 没有灯头，无法验证朝向'
            desired=math.radians(yaw-MODEL_YAW_OFFSET)
            for spot in spots:
                front=spot.get_forward_vector()
                assert front.x*math.cos(desired)+front.y*math.sin(desired)>0.999,label+': 灯头朝向校验失败'
            assert (a.get_actor_location()-common).length()<0.1,label
    if not dry_run and save and not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError('保存失败')
    with open(ROOT+'traffic_light_direction_fix_report.json','w',encoding='utf-8') as f:json.dump(report,f,ensure_ascii=False,indent=2)
    unreal.log('TRAFFIC_LIGHT_DIRECTION_FIX: '+str({k:v for k,v in report.items() if k!='rows'}))
    return report


if __name__=='__main__':
    try:main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
