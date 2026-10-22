# -*- coding: utf-8 -*-
"""共享灯位计算：在对应方向两条平行交通线之间寻找离所有路口线有余量的位置。"""
import math
import re
import unreal

WS = unreal.SplineCoordinateSpace.WORLD
NAME = re.compile(r'^Inter_(.+)_([+-]\d+)_I\d+$')
MIN_CLEARANCE = 80.0


def polyline(actor):
    sp = actor.get_component_by_class(unreal.SplineComponent)
    if not sp or sp.get_number_of_spline_points() < 2:
        return None
    length = sp.get_spline_length()
    n = max(1, math.ceil(length/100.0))
    return [sp.get_location_at_distance_along_spline(length*i/n, WS) for i in range(n+1)]


def crossing(points, center, dx, dy, station):
    """与垂直车道方向的断面求交，只接受线段内部，不外推到路口以外。"""
    hits = []
    for a,b in zip(points,points[1:]):
        da = (a.x-center.x)*dx+(a.y-center.y)*dy-station
        db = (b.x-center.x)*dx+(b.y-center.y)*dy-station
        if da*db > 0 or abs(db-da) < 0.00001:
            continue
        f = -da/(db-da)
        p = unreal.Vector(a.x+(b.x-a.x)*f,a.y+(b.y-a.y)*f,a.z+(b.z-a.z)*f)
        hits.append(p)
    return min(hits,key=lambda p:(p.x-center.x)**2+(p.y-center.y)**2) if hits else None


def distance_to_poly(p, points):
    best = float('inf')
    for a,b in zip(points,points[1:]):
        vx,vy = b.x-a.x,b.y-a.y
        length2 = vx*vx+vy*vy
        t = max(0.0,min(1.0,((p.x-a.x)*vx+(p.y-a.y)*vy)/length2)) if length2 else 0.0
        best = min(best,math.hypot(p.x-a.x-t*vx,p.y-a.y-t*vy))
    return best


def between_lanes(center, key, directions, actors_by_label):
    records = []
    for label,dx,dy,group,start in directions:
        match = NAME.match(label)
        actor = actors_by_label.get(label)
        points = polyline(actor) if actor else None
        if not match or not points:
            continue
        records.append(dict(label=label,road=match.group(1),offset=int(match.group(2)),dx=dx,dy=dy,
                            key=int(round(math.degrees(math.atan2(dy,dx))/90.0))%4,points=points))
    four_lane_roads={r['road'] for r in records
                     if r['road'].startswith('MainRoad') or abs(r['offset'])==540}
    facing = [r for r in records if r['key']==key]
    candidates = []
    for first in facing:
        for second in records:
            if first['label'] >= second['label'] and second['key']==key:
                continue
            if first is second or first['road'] != second['road']:
                continue
            # 有四车道主路时，整个路口只从主路两条内车道之间选公共灯位。
            # 严禁再取 -540/-180 或 +180/+540 这类同向内外车道的中间。
            if four_lane_roads:
                if first['road'] not in four_lane_roads:
                    continue
                if {first['offset'],second['offset']}!={-180,180}:
                    continue
            dot = first['dx']*second['dx']+first['dy']*second['dy']
            if abs(dot)<0.9:
                continue
            priority = 0 if four_lane_roads else (0 if dot>0 else 1)
            dx,dy = first['dx'],first['dy']
            for station in range(-800,801,100):
                a = crossing(first['points'],center,dx,dy,station)
                b = crossing(second['points'],center,dx,dy,station)
                if a is None or b is None:
                    continue
                gap = math.hypot(a.x-b.x,a.y-b.y)
                if not 180.0 <= gap <= 800.0:
                    continue
                p = unreal.Vector((a.x+b.x)/2,(a.y+b.y)/2,center.z)
                clearance = min(distance_to_poly(p,r['points']) for r in records)
                if clearance < MIN_CLEARANCE:
                    continue
                distance = math.hypot(p.x-center.x,p.y-center.y)
                candidates.append((priority,distance,-clearance,p,first['label'],second['label'],clearance))
    if not candidates:
        return None
    best = min(candidates,key=lambda c:c[:3])
    return best[3],best[4],best[5],best[6]


def shared_between_lanes(center, directions, actors_by_label):
    """一个路口只计算一个公共位置，所有朝向的灯共用相同 XYZ。"""
    choices = [between_lanes(center,key,directions,actors_by_label) for key in range(4)]
    choices = [c for c in choices if c is not None]
    return min(choices,key=lambda c:math.hypot(c[0].x-center.x,c[0].y-center.y)) if choices else None
