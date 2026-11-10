# -*- coding: utf-8 -*-
"""py connect_r037_s02_i03.py，执行后检查并手动保存，支持撤销"""
import unreal

WS = unreal.SplineCoordinateSpace.WORLD
LANE = 'Lane_BranchRoad_R037_-0180_S02'
INTER = 'Inter_BranchRoad_R037_-0180_I03'


def main():
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()
    def find(label):
        found = [a for a in actors if a.get_actor_label() == label]
        if len(found) != 1:
            raise RuntimeError('当前关卡必须恰好有一个 ' + label)
        return found[0]
    lane, inter = find(LANE), find(INTER)
    def spline(actor, name):
        return next(s for s in actor.get_components_by_class(unreal.SplineComponent)
                    if s.get_name() == name)
    src, dst = spline(lane, 'Spline'), spline(inter, 'SplineRight')
    n = src.get_number_of_spline_points()
    if n < 2 or dst.get_number_of_spline_points() < 2:
        raise RuntimeError('样条点不足，未修改')
    end = dst.get_location_at_spline_point(0, WS)
    direction = dst.get_tangent_at_spline_point(0, WS)
    if direction.length() < 0.01:
        raise RuntimeError('路口入口切线为零，请先修复右转线')
    direction = direction / direction.length()
    old_end = src.get_location_at_spline_point(n-1, WS)
    if (old_end-end).length() > 1500:
        raise RuntimeError('端点间距超过15米，未修改，请核对道路')
    # 保留末端约六米以前的点，只重建接缝附近
    anchor = n-2
    while anchor > 0 and (src.get_location_at_spline_point(anchor, WS)-end).length() < 600:
        anchor -= 1
    start = src.get_location_at_spline_point(anchor, WS)
    length = (end-start).length()
    if length < 1:
        raise RuntimeError('连接段过短，未修改')
    tangent = src.get_tangent_at_spline_point(anchor, WS)
    if tangent.length() < 0.01:
        tangent = end-start
    tangent = tangent / tangent.length() * length
    finish = direction * length
    boxes = inter.get_components_by_class(unreal.BoxComponent)
    if len(boxes) != 1:
        raise RuntimeError('路口入口 Box 数量不为一，未修改')
    box = boxes[0]
    if (box.get_world_location()-end).length() > 200:
        raise RuntimeError('入口 Box 距右转线起点超过2米，需检查路口布局，未修改')
    with unreal.ScopedEditorTransaction('平滑 R037 S02 到 I03 接缝'):
        lane.modify(True)
        src.modify(True)
        src.set_editor_property('spline_has_been_edited', True)
        for i in range(n-1, anchor, -1):
            src.remove_spline_point(i, False)
        arrive = src.get_arrive_tangent_at_spline_point(anchor, WS)
        src.add_spline_point(end, WS, False)
        src.set_tangents_at_spline_point(anchor, arrive, tangent, WS, False)
        src.set_tangents_at_spline_point(anchor+1, finish, finish, WS, False)
        src.update_spline()
    error = (src.get_location_at_spline_point(anchor+1, WS)-end).length()
    if error > 0.1:
        raise RuntimeError('接缝回读异常，请 Ctrl+Z 撤销')
    subsystem.set_selected_level_actors([lane, inter])
    unreal.log('[R037] 接缝已对齐，末段入口方向与右转线一致；原缺口 %.1f cm，现 %.3f cm。请 PIE 检查车辆后保存，可 Ctrl+Z 撤销' % ((old_end-end).length(), error))


main()
