# -*- coding: utf-8 -*-
"""py extend_light_splines_outside_roads.py：沿现有红线向路外调整并安全延长两端。"""
import datetime
import json
import math
import re
import traceback

import unreal

from cut_light_splines_at_roads import RoadQuery, backup_actor, write_points

DRY_RUN = False
FOLDER = 'light_spline'
TAG = 'DeliveryLightSplineOuterExtendedV1'
LANE_RE = re.compile(r'^Lane_(.+)_([+-]\d+)_S\d+$')
ORIGINAL_OFFSET = 410.0    # 原红线相对外侧车道的偏移
EXTRA_OUTWARD = 75.0       # 整条现有红线再向路外移75cm
MAX_EXTRA_OUTWARD = 350.0  # 局部路面较宽时最多再向外尝试这么远
SAMPLE_STEP = 50.0         # 保留现有曲线形状的采样精度
EDGE_SEARCH = 1100.0       # 只延长靠近源车道首尾的红线；路口裁剪口不强接
MATCH_TOLERANCE = 200.0
CHECK_STEP = 25.0
WS = unreal.SplineCoordinateSpace.WORLD
ROOT = unreal.Paths.project_saved_dir()


def point(sp,d):
    return sp.get_location_at_distance_along_spline(d,WS)


def outward(road,p,sign):
    key=road.find_input_key_closest_to_world_location(p)
    right=road.get_right_vector_at_spline_input_key(key,WS)
    size=math.hypot(right.x,right.y)
    if size<0.001:
        raise RuntimeError('道路右向量退化')
    return sign*right.x/size,sign*right.y/size


def shift_existing(p,road,sign,query):
    nx,ny=outward(road,p,sign)
    for amount in range(int(EXTRA_OUTWARD),int(MAX_EXTRA_OUTWARD)+1,25):
        q=unreal.Vector(p.x+nx*amount,p.y+ny*amount,p.z)
        if not query.on_road(q):
            return q
    return None


def lane_outer_point(lane,road,sign,d,query):
    p=point(lane,d)
    nx,ny=outward(road,p,sign)
    for amount in range(int(EXTRA_OUTWARD),int(MAX_EXTRA_OUTWARD)+1,25):
        offset=ORIGINAL_OFFSET+amount
        q=unreal.Vector(p.x+nx*offset,p.y+ny*offset,p.z+3.0)
        if not query.on_road(q):
            return q
    return None


def segment_clear(a,b,query):
    length=math.hypot(a.x-b.x,a.y-b.y)
    count=max(1,math.ceil(length/CHECK_STEP))
    for i in range(count+1):
        t=i/count
        p=unreal.Vector(a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t)
        if query.on_road(p):
            return False
    return True


def closest_edge_distance(lane,road,sign,target,at_start,query):
    length=lane.get_spline_length()
    lo,hi=(0.0,min(length,EDGE_SEARCH)) if at_start else (max(0.0,length-EDGE_SEARCH),length)
    count=max(1,math.ceil((hi-lo)/SAMPLE_STEP))
    best=None
    for i in range(count+1):
        d=lo+(hi-lo)*i/count
        q=lane_outer_point(lane,road,sign,d,query)
        if q is None:
            continue
        separation=math.hypot(target.x-q.x,target.y-q.y)
        if best is None or separation<best[0]:
            best=(separation,d)
    return best[1] if best and best[0]<=MATCH_TOLERANCE else None


def extend(pts,lane,road,sign,query):
    length=lane.get_spline_length()
    head=closest_edge_distance(lane,road,sign,pts[0],True,query)
    tail=closest_edge_distance(lane,road,sign,pts[-1],False,query)
    before=[]
    if head is not None:
        nearest=pts[0]
        d=head-SAMPLE_STEP
        while d>=0:
            q=lane_outer_point(lane,road,sign,d,query)
            if q is None or not segment_clear(q,nearest,query):
                break
            before.append(q)
            nearest=q
            d-=SAMPLE_STEP
        if d<0 and head>0:
            q=lane_outer_point(lane,road,sign,0.0,query)
            if q is not None and math.hypot(q.x-nearest.x,q.y-nearest.y)>1.0 and segment_clear(q,nearest,query):
                before.append(q)
    after=[]
    if tail is not None:
        nearest=pts[-1]
        d=tail+SAMPLE_STEP
        while d<=length:
            q=lane_outer_point(lane,road,sign,d,query)
            if q is None or not segment_clear(nearest,q,query):
                break
            after.append(q)
            nearest=q
            d+=SAMPLE_STEP
        if d>length and tail<length:
            q=lane_outer_point(lane,road,sign,length,query)
            if q is not None and math.hypot(q.x-nearest.x,q.y-nearest.y)>1.0 and segment_clear(nearest,q,query):
                after.append(q)
    return list(reversed(before))+pts+after,len(before),len(after)


def main(dry_run=DRY_RUN):
    editor=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if editor.get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate')
    eas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors=list(eas.get_all_level_actors())
    by_label={a.get_actor_label():a for a in actors}
    roads={a.get_actor_label().replace(' ',''):a for a in actors
           if any(f in str(a.get_folder_path()) for f in
                  ('PS2DEM/Splines/MainRoad','PS2DEM/Splines/BranchRoad'))}
    query=RoadQuery(actors)
    probes=[]
    for a in actors:
        if a.get_actor_label().startswith('Lane_'):
            sp=a.get_component_by_class(unreal.SplineComponent)
            if sp:
                probes.append(query.on_road(point(sp,sp.get_spline_length()/2)))
            if len(probes)>=32:
                break
    if not any(probes):
        raise RuntimeError('交通车道的路面检测全部失败；未修改场景')

    plans=[]
    skipped=[]
    already=0
    for a in actors:
        if not a.get_actor_label().startswith('light_spline_') or str(a.get_folder_path()).split('/')[0]!=FOLDER:
            continue
        tags=[str(t) for t in a.tags]
        if TAG in tags:
            already+=1
            continue
        source=next((t.split(':',1)[1] for t in tags if t.startswith('SourceLane:')),None)
        match=LANE_RE.match(source or '')
        lane=by_label.get(source)
        road_actor=roads.get(match.group(1)) if match else None
        sp=a.get_component_by_class(unreal.SplineComponent)
        lane_sp=lane.get_component_by_class(unreal.SplineComponent) if lane else None
        road=road_actor.get_component_by_class(unreal.SplineComponent) if road_actor else None
        if not sp or not lane_sp or not road or sp.is_closed_loop():
            skipped.append(dict(label=a.get_actor_label(),reason='缺少源车道/道路样条，或红线是闭合线'))
            continue
        sign=1 if int(match.group(2))>0 else -1
        length=sp.get_spline_length()
        count=max(1,math.ceil(length/SAMPLE_STEP))
        shifted=[shift_existing(point(sp,length*i/count),road,sign,query) for i in range(count+1)]
        if any(p is None for p in shifted) or any(not segment_clear(p,q,query) for p,q in zip(shifted,shifted[1:])):
            skipped.append(dict(label=a.get_actor_label(),reason='向外调整后仍有路面重叠；保留原线'))
            continue
        pts,front,back=extend(shifted,lane_sp,road,sign,query)
        if any(not segment_clear(p,q,query) for p,q in zip(pts,pts[1:])):
            skipped.append(dict(label=a.get_actor_label(),reason='延长后与路面重叠；保留原线'))
            continue
        plans.append((a,pts,backup_actor(a,sp),front,back))

    report=dict(dry_run=dry_run,road_components=query.count,already_done=already,
                changed=len(plans),extended=sum(front+back>0 for _,_,_,front,back in plans),
                skipped=skipped,rows=[dict(label=a.get_actor_label(),points=len(pts),
                    added_at_start=front,added_at_end=back) for a,pts,_,front,back in plans])
    if plans and not dry_run:
        stamp=datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
        backup=ROOT+'light_spline_outer_backup_'+stamp+'.json'
        with open(backup,'w',encoding='utf-8') as output:
            json.dump([old for _,_,old,_,_ in plans],output,ensure_ascii=False,indent=2)
        report['backup']=backup
        with unreal.ScopedEditorTransaction('红色样条沿道路外侧延长'):
            for a,pts,_,front,back in plans:
                write_points(a,pts)
                a.tags=list(a.tags)+[TAG]
        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('地图保存失败；备份在 '+backup)
    with open(ROOT+'light_spline_outer_report.json','w',encoding='utf-8') as output:
        json.dump(report,output,ensure_ascii=False,indent=2)
    unreal.log('LIGHT_SPLINE_OUTER_DONE: 调整%d条，延长%d条，跳过%d条，已做过%d条' %
               (len(plans),report['extended'],len(skipped),already))
    return report


if __name__=='__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
