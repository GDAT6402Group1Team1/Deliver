# -*- coding: utf-8 -*-
"""py reposition_traffic_lights.py：每个路口整组灯重合，移到两条交通线之间。"""
import json
import traceback
import importlib
import unreal
from gen_traffic_lights import child_directions, INTERSECTION_PREFIX
import traffic_light_placement
importlib.reload(traffic_light_placement)
from traffic_light_placement import shared_between_lanes

ONLY_LIGHT = ''  # 可填 TrafficLight_ion_28_3；会调整它所在路口的整组灯


def main(save=True):
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate')
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = list(eas.get_all_level_actors())
    by_label = {a.get_actor_label():a for a in actors}
    inters = [a for a in actors if a.get_class().get_name().startswith(INTERSECTION_PREFIX)]
    moved,skipped = [],[]
    groups = {}
    for a in actors:
        label = a.get_actor_label()
        if not label.startswith('TrafficLight_'):
            continue
        matches = [inter for inter in inters for key in range(4)
                   if label=='TrafficLight_%s_%d' % (inter.get_actor_label()[-6:],key)]
        if len(matches)!=1:
            skipped.append({'label':label,'reason':'无法唯一匹配路口'})
            continue
        inter=matches[0]
        groups.setdefault(inter.get_actor_label(), (inter,[]))[1].append(a)
    with unreal.ScopedEditorTransaction('交通灯移到两条交通线中间'):
        for inter,lights in groups.values():
            if ONLY_LIGHT and not any(a.get_actor_label()==ONLY_LIGHT for a in lights):
                continue
            dirs = child_directions(inter,actors)
            # 基准与生成器一致，重复执行不累积位移。
            shared_z = sum(a.get_actor_location().z for a in lights)/len(lights)
            center = unreal.Vector(sum(r[4].x for r in dirs)/len(dirs),
                                   sum(r[4].y for r in dirs)/len(dirs),shared_z) if dirs else None
            result = shared_between_lanes(center,dirs,by_label) if center else None
            if not result:
                skipped.append({'intersection':inter.get_actor_label(),'reason':'未找到公共安全灯位，整组不动'})
                continue
            point,lane_a,lane_b,clearance = result
            for a in lights:
                label=a.get_actor_label()
                old=a.get_actor_location()
                a.modify(True)
                a.set_actor_location(point,False,True)
                assert (a.get_actor_location()-point).length()<0.1, label
                moved.append({'label':label,'intersection':inter.get_actor_label(),
                              'between':[lane_a,lane_b],'clearance_cm':clearance,
                              'before':[old.x,old.y,old.z],'after':[point.x,point.y,point.z]})
            assert all((a.get_actor_location()-point).length()<0.1 for a in lights)
    if moved and save and not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError('地图保存失败')
    with open(unreal.Paths.project_saved_dir()+'reposition_traffic_lights.json','w',encoding='utf-8') as f:
        json.dump({'moved':moved,'skipped':skipped},f,ensure_ascii=False,indent=2)
    unreal.log('TRAFFIC_LIGHT_REPOSITION_DONE: 移动%d，跳过%d，保存=%s' % (len(moved),len(skipped),save))
    for row in skipped:
        unreal.log_warning(str(row))
    return {'moved':moved,'skipped':skipped}


if __name__ == '__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
