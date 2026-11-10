# -*- coding: utf-8 -*-
"""在编辑器执行 py copy_r037_right_spline.py，检查后手动保存地图"""
import unreal


LABEL = "Inter_BranchRoad_R037_-0180_I03"
WS = unreal.SplineCoordinateSpace.WORLD


def main():
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    matches = [a for a in actors.get_all_level_actors()
               if a.get_actor_label() == LABEL]
    if len(matches) != 1:
        raise RuntimeError("当前关卡必须恰好有一个 " + LABEL)
    actor = matches[0]
    splines = {s.get_name(): s for s in
               actor.get_components_by_class(unreal.SplineComponent)}
    for name in ("Spline", "SplineLeft", "SplineRight"):
        if name not in splines:
            raise RuntimeError("缺少组件 " + name)
    source = splines["SplineRight"]
    count = source.get_number_of_spline_points()
    if count < 2:
        raise RuntimeError("右转线不足两个点，未修改")
    # 先读取源线，写入时不改变右转线
    points = [(source.get_location_at_spline_point(i, WS),
               source.get_arrive_tangent_at_spline_point(i, WS),
               source.get_leave_tangent_at_spline_point(i, WS),
               source.get_rotation_at_spline_point(i, WS),
               source.get_scale_at_spline_point(i),
               source.get_spline_point_type(i)) for i in range(count)]
    with unreal.ScopedEditorTransaction("R037 直行和左转复制右转样条"):
        actor.modify(True)
        for name in ("Spline", "SplineLeft"):
            target = splines[name]
            target.modify(True)
            # 保留实例编辑，避免构造脚本恢复默认点
            target.set_editor_property("spline_has_been_edited", True)
            target.clear_spline_points(False)
            for p, arrive, leave, rotation, scale, kind in points:
                target.add_spline_point(p, WS, False)
            for i, (p, arrive, leave, rotation, scale, kind) in enumerate(points):
                target.set_rotation_at_spline_point(i, rotation, WS, False)
                target.set_scale_at_spline_point(i, scale, False)
                target.set_tangents_at_spline_point(i, arrive, leave, WS, False)
                target.set_spline_point_type(i, kind, False)
            target.set_closed_loop(source.is_closed_loop(), False)
            target.update_spline()
            for step in range(101):
                distance = source.get_spline_length() * step / 100.0
                error = (target.get_location_at_distance_along_spline(distance, WS)
                         - source.get_location_at_distance_along_spline(distance, WS)).length()
                if error > 0.1:
                    raise RuntimeError(name + " 回读不一致，请撤销本次操作")
    actors.set_selected_level_actors([actor])
    unreal.log("[R037] Spline、SplineLeft 已复制为 SplineRight，采样核对通过；请检查后保存地图，可 Ctrl+Z 撤销")


main()
