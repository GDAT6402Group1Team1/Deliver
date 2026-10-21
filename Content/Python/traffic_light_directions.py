# -*- coding: utf-8 -*-
"""按进入方向成对归一灯朝向和相位，不改交通车道数据。"""
import math
from collections import Counter

MODEL_YAW_OFFSET = 90.0  # 已查组件：SpotLight 的本地朝向为 -Y，而非 Actor +X
GREEN_FIRST_GROUP = 0  # 上下(X/组0)绿起始；左右(Y/组1)红起始


def normalized_directions(dirs):
    buckets={}
    for label,dx,dy,group,start in dirs:
        key=int(round(math.degrees(math.atan2(dy,dx))/90.0))%4
        try: group=int(group)%2
        except (TypeError,ValueError):
            raise RuntimeError('读不到车道灯态分组: '+label)
        buckets.setdefault(key,[]).append((label,dx,dy,group))
    result={}
    warnings=[]
    for axis in (0,1):
        rows=buckets.get(axis,[])+buckets.get(axis+2,[])
        if not rows: continue
        counts=Counter(r[3] for r in rows)
        group=sorted(counts,key=lambda g:(-counts[g],g))[0]
        if len(counts)>1:
            warnings.append('方向对%d/%d车道分组冲突%s；灯按多数组%d统一，车道数据未改' % (axis,axis+2,dict(counts),group))
        # 对向翻转后求平均；结果强制互差180度，避免弯曲T口各用首条线造成偏差。
        dx=dy=0.0
        for label,x,y,g in rows:
            key=int(round(math.degrees(math.atan2(y,x))/90.0))%4
            sign=1 if key==axis else -1
            dx+=x*sign; dy+=y*sign
        if math.hypot(dx,dy)<0.01:
            raise RuntimeError('无法确定方向对 %d/%d' % (axis,axis+2))
        yaw=math.degrees(math.atan2(dy,dx))
        for key in (axis,axis+2):
            if key in buckets:
                result[key]=dict(travel_yaw=yaw+(180 if key!=axis else 0),group=group)
    return result,warnings
