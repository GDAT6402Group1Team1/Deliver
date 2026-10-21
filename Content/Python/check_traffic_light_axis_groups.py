# -*- coding: utf-8 -*-
"""只读核对：左右(Y)为1，上下(X)为0。py check_traffic_light_axis_groups.py"""
import json
import math
import unreal
from gen_traffic_lights import read_prop


def main():
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    rows=[];lights=[]
    ws=unreal.SplineCoordinateSpace.WORLD
    for a in actors:
        label=a.get_actor_label()
        if label.startswith('Inter_'):
            sp=a.get_component_by_class(unreal.SplineComponent)
            if not sp or sp.get_number_of_spline_points()<2:continue
            length=sp.get_spline_length()
            start=sp.get_location_at_distance_along_spline(0,ws)
            end=sp.get_location_at_distance_along_spline(length,ws)
            near=sp.get_location_at_distance_along_spline(min(200,length),ws)
            dx,dy=end.x-start.x,end.y-start.y
            expected=1 if abs(dy)>abs(dx) else 0
            actual=int(read_prop(a,'LightNumber'))
            entry_group=1 if abs(near.y-start.y)>abs(near.x-start.x) else 0
            angle=math.degrees(math.atan2(abs(dy),abs(dx)))
            rows.append(dict(label=label,dx=dx,dy=dy,expected=expected,actual=actual,
                             matches=actual==expected,near_diagonal=abs(angle-45)<10,
                             entry_expected=entry_group,entry_axis_differs=entry_group!=expected))
        elif label.startswith('TrafficLight_'):
            tags=[str(t) for t in a.tags]
            tag=next((t for t in tags if t.startswith('ClaudeGenTrafficLight:')),None)
            if tag:
                key=int(label.rsplit('_',1)[1])
                expected=key%2
                actual=int(tag.rsplit(':',1)[1])
                lights.append(dict(label=label,key=key,expected=expected,actual=actual,matches=actual==expected))
    report=dict(rule='|dy|>|dx| -> 1; otherwise 0',inter_segments=len(rows),
                segment_matches=sum(r['matches'] for r in rows),
                mismatches=[r for r in rows if not r['matches']],
                entry_axis_differs=[r for r in rows if r['entry_axis_differs']],
                near_diagonal=[r for r in rows if r['near_diagonal']],
                lights=len(lights),light_matches=sum(r['matches'] for r in lights),
                light_mismatches=[r for r in lights if not r['matches']])
    with open(unreal.Paths.project_saved_dir()+'traffic_axis_group_check.json','w',encoding='utf-8') as f:
        json.dump(report,f,ensure_ascii=False,indent=2)
    unreal.log('TRAFFIC_AXIS_CHECK: '+str({k:v for k,v in report.items() if not isinstance(v,list)}))
    return report


if __name__=='__main__':main()
