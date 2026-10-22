# -*- coding: utf-8 -*-
"""py cut_light_splines_at_roads.py：删除红线进入真实路面的部分，中途重合则拆段。"""
import datetime
import json
import math
import traceback
import unreal

DRY_RUN = False
ROAD_MESH = '/Game/model/road_0.road_0'
STEP = 25.0                # 每25cm检查曲线内部，不只查控制点
HEIGHT_WINDOW = 300.0      # 排除桥下/桥上的另一层路
BOUNDARY_INSET = 5.0       # 保留段在切口处额外退5cm
GRID = 2000.0
WS = unreal.SplineCoordinateSpace.WORLD
ROOT = unreal.Paths.project_saved_dir()


class RoadQuery:
    def __init__(self,actors):
        self.cells={}
        self.count=0
        for a in actors:
            for c in a.get_components_by_class(unreal.SplineMeshComponent):
                m=c.get_editor_property('static_mesh')
                if not m or m.get_path_name()!=ROAD_MESH:
                    continue
                origin,extent,radius=unreal.SystemLibrary.get_component_bounds(c)
                bounds=(origin.x-extent.x,origin.x+extent.x,origin.y-extent.y,origin.y+extent.y)
                self.count+=1
                for x in range(math.floor(bounds[0]/GRID),math.floor(bounds[1]/GRID)+1):
                    for y in range(math.floor(bounds[2]/GRID),math.floor(bounds[3]/GRID)+1):
                        self.cells.setdefault((x,y),[]).append((c,bounds))
        if not self.count:
            raise RuntimeError('没有找到真实道路网格 '+ROAD_MESH+'；停止，未修改场景')

    def on_road(self,p):
        start=unreal.Vector(p.x,p.y,p.z+HEIGHT_WINDOW)
        end=unreal.Vector(p.x,p.y,p.z-HEIGHT_WINDOW)
        for c,b in self.cells.get((math.floor(p.x/GRID),math.floor(p.y/GRID)),[]):
            if not (b[0]<=p.x<=b[1] and b[2]<=p.y<=b[3]):
                continue
            # 直接查道路组件，不会被地形/裙边/箱子挡住，也不依赖世界碰撞通道。
            hit=c.line_trace_component(start,end,True,False,False)
            if hit is None:
                continue
            # UE Python: bool 返回值折入输出元组；命中时返回输出值，未命中返回 None。
            if isinstance(hit,tuple):
                if hit and isinstance(hit[0],bool):
                    if hit[0]: return True
                else:
                    # K2_LineTraceComponent 成功返回 (HitLocation,HitNormal,BoneName,OutHit)，
                    # 这个接口的 OutHit 在当前版本可能为空，成功以非 None 输出元组为准。
                    if len(hit)==4: return True
                    raise RuntimeError('不支持的 LineTraceComponent 输出: '+repr(hit))
            else:
                raise RuntimeError('不支持的 LineTraceComponent 返回类型: '+str(type(hit)))
        return False


def point(sp,d):
    return sp.get_location_at_distance_along_spline(d,WS)


def split_ranges(sp,query):
    total=sp.get_spline_length()
    n=max(1,math.ceil(total/STEP))
    ds=[total*i/n for i in range(n+1)]
    states=[query.on_road(point(sp,d)) for d in ds]
    if not any(states): return None
    keep=[]
    begin=0.0 if not states[0] else None
    for i in range(1,len(ds)):
        if states[i]==states[i-1]: continue
        lo,hi=ds[i-1],ds[i]
        old=states[i-1]
        for _ in range(10):
            mid=(lo+hi)/2
            if query.on_road(point(sp,mid))==old: lo=mid
            else: hi=mid
        boundary=(lo+hi)/2
        if old: begin=boundary+BOUNDARY_INSET
        elif begin is not None:
            end=boundary-BOUNDARY_INSET
            if end-begin>=1.0: keep.append((begin,end))
            begin=None
    if begin is not None and total-begin>=1.0: keep.append((begin,total))
    return keep


def backup_actor(a,sp):
    rows=[]
    for i in range(sp.get_number_of_spline_points()):
        p=sp.get_location_at_spline_point(i,WS)
        arrive=sp.get_arrive_tangent_at_spline_point(i,WS)
        leave=sp.get_leave_tangent_at_spline_point(i,WS)
        rotation=sp.get_rotation_at_spline_point(i,WS)
        scale=sp.get_scale_at_spline_point(i)
        rows.append(dict(position=[p.x,p.y,p.z],arrive=[arrive.x,arrive.y,arrive.z],
                         leave=[leave.x,leave.y,leave.z],type=str(sp.get_spline_point_type(i)),
                         rotation=[rotation.roll,rotation.pitch,rotation.yaw],scale=[scale.x,scale.y,scale.z]))
    return dict(label=a.get_actor_label(),actor_path=a.get_path_name(),
                cls=a.get_class().get_path_name(),folder=str(a.get_folder_path()),
                tags=[str(t) for t in a.tags],closed=sp.is_closed_loop(),points=rows)


def write_points(a,pts):
    a.modify(True)
    sp=a.get_component_by_class(unreal.SplineComponent)
    sp.set_editor_property('spline_has_been_edited',True)
    sp=a.get_component_by_class(unreal.SplineComponent)
    sp.modify(True)
    sp.set_closed_loop(False,False)
    sp.clear_spline_points(False)
    for p in pts: sp.add_spline_point(p,WS,False)
    # 25cm密集折线贴住原曲线，避免切口自动切线重新鼓进马路。
    for i in range(len(pts)): sp.set_spline_point_type(i,unreal.SplinePointType.LINEAR,False)
    red=unreal.LinearColor(1,0,0,1)
    sp.set_unselected_spline_segment_color(red)
    sp.set_selected_spline_segment_color(red)
    sp.set_tangent_color(red)
    sp.set_draw_debug(True)
    sp.update_spline()
    assert sp.get_number_of_spline_points()==len(pts)
    for i,p in enumerate(pts): assert (point_at_index(sp,i)-p).length()<0.1


def point_at_index(sp,i):
    return sp.get_location_at_spline_point(i,WS)


def main(dry_run=DRY_RUN):
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请停止 PIE / Simulate 后执行')
    eas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors=list(eas.get_all_level_actors())
    query=RoadQuery(actors)
    probes=[]
    for a in actors:
        if a.get_actor_label().startswith('Lane_'):
            s=a.get_component_by_class(unreal.SplineComponent)
            if s:
                probes.append(query.on_road(point(s,s.get_spline_length()/2)))
            if len(probes)>=32: break
    if not probes or not any(probes):
        raise RuntimeError('已知交通车道的路面检测全部失败，停止，未修改场景')
    plans=[]
    unchanged=0
    for a in actors:
        if not a.get_actor_label().startswith('light_spline_') or str(a.get_folder_path()).split('/')[0]!='light_spline':
            continue
        sp=a.get_component_by_class(unreal.SplineComponent)
        if not sp: continue
        if sp.is_closed_loop():
            raise RuntimeError(a.get_actor_label()+': 闭合线需单独处理，未修改场景')
        ranges=split_ranges(sp,query)
        if ranges is None:
            unchanged+=1
            continue
        segments=[]
        for start,end in ranges:
            n=max(1,math.ceil((end-start)/STEP))
            pts=[point(sp,start+(end-start)*i/n) for i in range(n+1)]
            # 验证输出折线的端点和每段中点没有重新切进路面。
            if any(query.on_road(p) for p in pts) or any(query.on_road((p+q)*0.5) for p,q in zip(pts,pts[1:])):
                raise RuntimeError(a.get_actor_label()+': 切口验证失败，停止，未修改场景')
            segments.append(pts)
        plans.append((a,segments,backup_actor(a,sp)))
    report=dict(dry_run=dry_run,road_components=query.count,unchanged=unchanged,
                changed=len(plans),fully_removed=sum(not p[1] for p in plans),
                split=sum(len(p[1])>1 for p in plans),
                rows=[dict(label=a.get_actor_label(),remaining_parts=len(parts)) for a,parts,b in plans])
    if not dry_run and plans:
        stamp=datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
        backup=ROOT+'light_spline_road_cut_backup_'+stamp+'.json'
        # 先完成全部计算和备份，再开始改场景。
        with open(backup,'w',encoding='utf-8') as f: json.dump([p[2] for p in plans],f,ensure_ascii=False,indent=2)
        report['backup']=backup
        labels={a.get_actor_label() for a in actors}
        with unreal.ScopedEditorTransaction('剪掉红色样条与路面重合的部分'):
            for a,parts,b in plans:
                for i,pts in enumerate(parts):
                    target=a if i==0 else eas.spawn_actor_from_class(a.get_class(),pts[0])
                    if i:
                        base=b['label']+'_Cut%02d' % (i+1)
                        label=base
                        suffix=1
                        while label in labels:
                            label=base+'_%02d'%suffix; suffix+=1
                        labels.add(label)
                        target.set_actor_label(label)
                        target.set_folder_path(b['folder'])
                        target.tags=b['tags']
                    write_points(target,pts)
                if not parts:
                    if not eas.destroy_actor(a): raise RuntimeError('删除完全重合样条失败: '+b['label'])
        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('保存地图失败，备份在 '+backup)
    with open(ROOT+'light_spline_road_cut_report.json','w',encoding='utf-8') as f:
        json.dump(report,f,ensure_ascii=False,indent=2)
    unreal.log('LIGHT_ROAD_CUT: '+str({k:v for k,v in report.items() if k!='rows'}))
    return report


if __name__=='__main__':
    try: main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
