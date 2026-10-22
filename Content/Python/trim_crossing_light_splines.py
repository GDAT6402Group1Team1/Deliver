# -*- coding: utf-8 -*-
"""py trim_crossing_light_splines.py：红线首尾各删2点，交叉处双方各删4点并断开。"""
import datetime
import json
import math
import traceback

import unreal

from cut_light_splines_at_roads import RoadQuery, backup_actor, write_points

DONE_TAG = 'DeliveryLightSplineNoCrossV1'
FOLDER = 'light_spline'
GRID = 500.0
ROAD_STEP = 25.0
WS = unreal.SplineCoordinateSpace.WORLD
ROOT = unreal.Paths.project_saved_dir()


def xy_cross(ax,ay,bx,by):
    return ax*by-ay*bx


def segment_intersection(a,b,c,d):
    """返回两条水平线段上的比例；端点相接及共线重叠也算相交。"""
    rx,ry=b.x-a.x,b.y-a.y
    sx,sy=d.x-c.x,d.y-c.y
    qx,qy=c.x-a.x,c.y-a.y
    denominator=xy_cross(rx,ry,sx,sy)
    if abs(denominator)>1e-8:
        t=xy_cross(qx,qy,sx,sy)/denominator
        u=xy_cross(qx,qy,rx,ry)/denominator
        if -1e-7<=t<=1+1e-7 and -1e-7<=u<=1+1e-7:
            return max(0.0,min(1.0,t)),max(0.0,min(1.0,u))
        return None
    if abs(xy_cross(qx,qy,rx,ry))>1e-5:
        return None
    rr=rx*rx+ry*ry
    ss=sx*sx+sy*sy
    if rr<1e-8 or ss<1e-8:
        return None
    t0=(qx*rx+qy*ry)/rr
    t1=((d.x-a.x)*rx+(d.y-a.y)*ry)/rr
    lo=max(0.0,min(t0,t1))
    hi=min(1.0,max(t0,t1))
    if lo>hi+1e-7:
        return None
    t=(lo+hi)/2.0
    x,y=a.x+t*rx,a.y+t*ry
    u=((x-c.x)*sx+(y-c.y)*sy)/ss
    return t,max(0.0,min(1.0,u))


def live_segments(rows):
    segments=[]
    for row_id,row in enumerate(rows):
        pts=row['points']
        removed=row['removed']
        for i in range(len(pts)-1):
            if i in removed or i+1 in removed:
                continue
            a,b=pts[i],pts[i+1]
            if (a.x-b.x)**2+(a.y-b.y)**2<1e-8:
                continue
            segments.append((row_id,i,a,b))
    return segments


def crossings(segments):
    cells={}
    for sid,(_,_,a,b) in enumerate(segments):
        for x in range(math.floor(min(a.x,b.x)/GRID),math.floor(max(a.x,b.x)/GRID)+1):
            for y in range(math.floor(min(a.y,b.y)/GRID),math.floor(max(a.y,b.y)/GRID)+1):
                cells.setdefault((x,y),[]).append(sid)
    seen=set()
    hits=[]
    for ids in cells.values():
        for pos,left_id in enumerate(ids):
            left=segments[left_id]
            for right_id in ids[pos+1:]:
                key=(min(left_id,right_id),max(left_id,right_id))
                if key in seen:
                    continue
                seen.add(key)
                right=segments[right_id]
                if left[0]==right[0] and abs(left[1]-right[1])<=1:
                    continue
                crossing=segment_intersection(left[2],left[3],right[2],right[3])
                if crossing is not None:
                    hits.append((left,right,crossing))
    return hits


def crossing_groups(hits):
    """同一个顶点可能由四对相邻线段同时报出，合成一个实际交点。"""
    groups={}
    for left,right,(t,u) in hits:
        a,b=left[2],left[3]
        x,y=a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t
        key=(round(x/10.0),round(y/10.0))
        group=groups.setdefault(key,{})
        group.setdefault(left[0],set()).add(left[1])
        group.setdefault(right[0],set()).add(right[1])
    affected=set()
    for group in groups.values():
        for row_id,indices in group.items():
            ordered=sorted(indices)
            runs=[]
            for index in ordered:
                if not runs or index>runs[-1][-1]+1:
                    runs.append([index])
                else:
                    runs[-1].append(index)
            for run in runs:
                affected.add((row_id,run[len(run)//2]))
    return len(groups),affected


def on_road_segments(segments,road):
    hits=[]
    for segment in segments:
        _,_,a,b=segment
        length=math.hypot(a.x-b.x,a.y-b.y)
        count=max(1,math.ceil(length/ROAD_STEP))
        for j in range(count+1):
            t=j/count
            p=unreal.Vector(a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t)
            if road.on_road(p):
                hits.append(segment)
                break
    return hits


def remove_four(row,segment_index):
    center=segment_index+0.5
    available=[i for i in range(len(row['points'])) if i not in row['removed']]
    picked=sorted(available,key=lambda i:(abs(i-center),i))[:4]
    row['removed'].update(picked)
    return len(picked)


def remaining_runs(row):
    runs=[]
    current=[]
    for i,p in enumerate(row['points']):
        if i in row['removed']:
            if len(current)>=2:
                runs.append(current)
            current=[]
        else:
            current.append(p)
    if len(current)>=2:
        runs.append(current)
    return runs


def main():
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate')
    eas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors=list(eas.get_all_level_actors())
    rows=[]
    for a in actors:
        if not a.get_actor_label().startswith('light_spline_') or str(a.get_folder_path()).split('/')[0]!=FOLDER:
            continue
        sp=a.get_component_by_class(unreal.SplineComponent)
        if not sp or sp.is_closed_loop():
            raise RuntimeError(a.get_actor_label()+': 缺少样条或为闭合线；未修改关卡')
        points=[sp.get_location_at_spline_point(i,WS)
                for i in range(sp.get_number_of_spline_points())]
        tags=[str(tag) for tag in a.tags]
        removed=set()
        if DONE_TAG not in tags:
            removed.update(range(min(2,len(points))))
            removed.update(range(max(0,len(points)-2),len(points)))
        rows.append(dict(actor=a,label=a.get_actor_label(),points=points,tags=tags,
                         removed=removed,initial_trim=len(removed),
                         backup=backup_actor(a,sp)))
    if not rows:
        raise RuntimeError('没有找到 light_spline 文件夹中的红线')
    rows.sort(key=lambda row:row['label'])
    road=RoadQuery(actors)
    probes=[]
    for a in actors:
        if a.get_actor_label().startswith('Lane_'):
            sp=a.get_component_by_class(unreal.SplineComponent)
            if sp:
                probes.append(road.on_road(sp.get_location_at_distance_along_spline(sp.get_spline_length()/2,WS)))
            if len(probes)>=32:
                break
    if not any(probes):
        raise RuntimeError('道路重叠检测失效；未修改关卡')

    initial_crossings=None
    crossing_events=0
    road_events=0
    for iteration in range(100):
        segments=live_segments(rows)
        pair_hits=crossings(segments)
        crossing_count,crossing_affected=crossing_groups(pair_hits)
        road_hits=on_road_segments(segments,road)
        if initial_crossings is None:
            initial_crossings=crossing_count
        if not pair_hits and not road_hits:
            break
        crossing_events+=crossing_count
        road_events+=len(road_hits)
        affected=set(crossing_affected)
        affected.update((s[0],s[1]) for s in road_hits)
        before=sum(len(row['removed']) for row in rows)
        for row_id,point_index in affected:
            remove_four(rows[row_id],point_index)
        if sum(len(row['removed']) for row in rows)==before:
            raise RuntimeError('交叉点无法继续删点；未修改关卡')
    else:
        raise RuntimeError('100轮仍存在红线交叉；未修改关卡')

    plans=[]
    for row in rows:
        runs=remaining_runs(row)
        if row['removed'] or DONE_TAG not in row['tags']:
            plans.append((row,runs))
    report=dict(red_lines=len(rows),initial_crossings=initial_crossings,
                crossing_events=crossing_events,road_overlap_events=road_events,
                remaining_crossings=0,remaining_road_overlaps=0,
                changed=len(plans),removed_actors=sum(not runs for _,runs in plans),
                split_actors=sum(len(runs)>1 for _,runs in plans),
                rows=[dict(label=row['label'],points_before=len(row['points']),
                           initial_trim=row['initial_trim'],
                           points_removed=len(row['removed']),parts=len(runs))
                      for row,runs in plans])
    if plans:
        stamp=datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
        backup=ROOT+'light_spline_no_cross_backup_'+stamp+'.json'
        with open(backup,'w',encoding='utf-8') as output:
            json.dump([row['backup'] for row,_ in plans],output,ensure_ascii=False,indent=2)
        report['backup']=backup
        labels={a.get_actor_label() for a in actors}
        with unreal.ScopedEditorTransaction('红线首尾删点并断开交叉处'):
            for row,runs in plans:
                source=row['actor']
                if not runs:
                    if not eas.destroy_actor(source):
                        raise RuntimeError('删除过短红线失败: '+row['label'])
                    continue
                for part,pts in enumerate(runs):
                    target=source if part==0 else eas.spawn_actor_from_class(source.get_class(),pts[0])
                    if not target:
                        raise RuntimeError('分段生成失败: '+row['label'])
                    if part:
                        base=row['label']+'_NoCross%02d'%(part+1)
                        label=base
                        suffix=1
                        while label in labels:
                            label=base+'_%02d'%suffix
                            suffix+=1
                        labels.add(label)
                        target.set_actor_label(label)
                        target.set_folder_path(FOLDER)
                    write_points(target,pts)
                    target.tags=list(dict.fromkeys(row['tags']+[DONE_TAG]))
        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('地图保存失败；备份在 '+backup)
    with open(ROOT+'light_spline_no_cross_report.json','w',encoding='utf-8') as output:
        json.dump(report,output,ensure_ascii=False,indent=2)
    unreal.log('LIGHT_SPLINE_NO_CROSS_DONE: 原交叉%d处，修改%d条，断开%d条，删除%d条' %
               (initial_crossings,len(plans),report['split_actors'],report['removed_actors']))
    return report


if __name__=='__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
