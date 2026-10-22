# -*- coding: utf-8 -*-
"""在当前编辑器地图生成外侧车道的红色路灯路径。py gen_light_splines.py"""
import json
import math
import re
import traceback
import unreal

BP = '/Game/Blueprint/Environment/BP_LightSpline'
TAG = 'DeliveryLightSpline'
TRIM_TAG = 'DeliveryLightSplineTrim2EachEnd'
TRIM_END_POINTS = 2
FOLDER = 'light_spline'
OUTWARD_OFFSET = 410.0  # 主路/支路均为标称路缘外 50cm
SAMPLE_STEP = 200.0
ONLY_SOURCE = ''  # 留空处理全部；可填 Lane_MainRoad_R003_-0540_S05 只生成示例
WS = unreal.SplineCoordinateSpace.WORLD
PATTERN = re.compile(r'^Lane_(.+)_([+-]\d+)_S(\d+)$')
REPORT = unreal.Paths.project_saved_dir() + 'light_splines.json'


def blueprint_class():
    bp = unreal.load_asset(BP)
    if not bp:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property('parent_class', unreal.Actor)
        folder, name = BP.rsplit('/', 1)
        bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.Blueprint, factory)
        sub = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
        handles = sub.k2_gather_subobject_data_for_blueprint(bp)
        handle, reason = sub.add_new_subobject(unreal.AddNewSubobjectParams(
            parent_handle=handles[0], new_class=unreal.SplineComponent, blueprint_context=bp))
        if str(reason):
            raise RuntimeError('添加样条组件失败: ' + str(reason))
        sub.rename_subobject(handle, 'LightSpline')
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)
        unreal.EditorAssetLibrary.save_loaded_asset(bp)
    cls = unreal.EditorAssetLibrary.load_blueprint_class(BP)
    if not cls:
        raise RuntimeError('加载路灯样条蓝图类失败')
    return cls


def red(sp):
    color = unreal.LinearColor(1.0, 0.0, 0.0, 1.0)
    sp.set_unselected_spline_segment_color(color)
    sp.set_selected_spline_segment_color(color)
    sp.set_tangent_color(color)
    sp.set_draw_debug(True)


def main():
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate，再生成路灯样条')
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = list(eas.get_all_level_actors())
    roads = {a.get_actor_label().replace(' ', ''): a for a in actors
             if any(f in str(a.get_folder_path()) for f in
                    ('PS2DEM/Splines/MainRoad', 'PS2DEM/Splines/BranchRoad'))}
    planned = []
    for a in actors:
        if ONLY_SOURCE and a.get_actor_label() != ONLY_SOURCE:
            continue
        match = PATTERN.match(a.get_actor_label())
        if not match:
            continue
        road_name, offset, segment = match.groups()
        outer = 540 if road_name.startswith('MainRoad') else 180 if road_name.startswith('BranchRoad') else None
        if outer is None or abs(int(offset)) != outer:
            continue
        road = roads.get(road_name)
        if not road:
            raise RuntimeError('找不到源道路: ' + road_name)
        rs = road.get_component_by_class(unreal.SplineComponent)
        sp = a.get_component_by_class(unreal.SplineComponent)
        if not rs or not sp:
            raise RuntimeError('源道路或车道缺少样条组件: ' + a.get_actor_label())
        length = sp.get_spline_length()
        if length < 1.0:
            raise RuntimeError('车道过短: ' + a.get_actor_label())
        count = max(1, math.ceil(length / SAMPLE_STEP))
        pts = []
        for i in range(count + 1):
            distance = length * i / count
            p = sp.get_location_at_distance_along_spline(distance, WS)
            # 用源道路右向量与命名偏移符号确认朝外，不依赖车道行驶点序。
            key = rs.find_input_key_closest_to_world_location(p)
            right = rs.get_right_vector_at_spline_input_key(key, WS)
            sign = 1.0 if int(offset) > 0 else -1.0
            t = sp.get_direction_at_distance_along_spline(distance, WS)
            nx, ny = -t.y, t.x
            norm = math.hypot(nx, ny)
            if norm < 0.0001:
                before = sp.get_location_at_distance_along_spline(max(0.0, distance-10.0), WS)
                after = sp.get_location_at_distance_along_spline(min(length, distance+10.0), WS)
                nx, ny = -(after.y-before.y), after.x-before.x
                norm = math.hypot(nx, ny)
                if norm < 0.0001:
                    raise RuntimeError('退化切线: ' + a.get_actor_label())
            nx, ny = nx/norm, ny/norm
            if (nx*right.x + ny*right.y)*sign < 0:
                nx, ny = -nx, -ny
            # 保留已贴路面的车道高度，路径只用于编辑器布灯，不改变交通高度。
            pts.append(unreal.Vector(p.x + nx*OUTWARD_OFFSET,
                                     p.y + ny*OUTWARD_OFFSET, p.z + 3.0))
        if len(pts) >= TRIM_END_POINTS*2 + 2:
            pts = pts[TRIM_END_POINTS:-TRIM_END_POINTS]
        else:
            unreal.log_warning(a.get_actor_label() + ': 不足6点，保留短段，不裁剪')
        planned.append((a, pts, count+1 >= TRIM_END_POINTS*2+2))
    if not planned:
        raise RuntimeError('当前地图未找到外侧车道，未修改地图')
    cls = blueprint_class()
    existing = {}
    for a in actors:
        if TAG in [str(t) for t in a.tags]:
            label = a.get_actor_label()
            if label in existing:
                raise RuntimeError('生成物重名: ' + label)
            existing[label] = a
    report = {'offset_cm': OUTWARD_OFFSET, 'height_mode': 'lane_z_plus_3cm', 'splines': []}
    with unreal.ScopedEditorTransaction('生成路外红色 light_spline'):
        for source, pts, trimmed in planned:
            label = 'light_spline_' + source.get_actor_label()[5:]
            a = existing.get(label) or eas.spawn_actor_from_class(cls, pts[0])
            a.modify(True)
            a.set_actor_label(label)
            a.set_folder_path(FOLDER)
            a.tags = [TAG, 'SourceLane:' + source.get_actor_label()] + ([TRIM_TAG] if trimmed else [])
            sp = a.get_component_by_class(unreal.SplineComponent)
            if not sp:
                raise RuntimeError('生成蓝图缺少 SplineComponent')
            # 必须在写点之前打开实例覆盖。set_editor_property 会触发蓝图组件
            # 重建；写点之后才设属性会让第一轮点数据被默认两点覆盖。
            sp.set_editor_property('spline_has_been_edited', True)
            sp = a.get_component_by_class(unreal.SplineComponent)
            sp.modify(True)
            sp.clear_spline_points(False)
            for p in pts:
                sp.add_spline_point(p, WS, False)
            for i in range(len(pts)):
                sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
            for i in (0, len(pts)-1):
                sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
            red(sp)
            sp.update_spline()
            actual = a.get_component_by_class(unreal.SplineComponent)
            if actual.get_number_of_spline_points() != len(pts):
                raise RuntimeError(label + ': 写入后点数不符，停止保存')
            for i, expected in enumerate(pts):
                got = actual.get_location_at_spline_point(i, WS)
                if (got-expected).length() > 0.1:
                    raise RuntimeError(label + ': 写入后位置不符，停止保存')
            report['splines'].append({'source':source.get_actor_label(), 'label':label,
                'points':len(pts), 'first':[pts[0].x,pts[0].y,pts[0].z],
                'last':[pts[-1].x,pts[-1].y,pts[-1].z]})
    # 只保存当前地图及新蓝图；当前编辑器的原有地图修改也一并保留。
    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError('地图保存失败')
    with open(REPORT, 'w', encoding='utf-8') as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    target = next((a for a in eas.get_all_level_actors()
                   if a.get_actor_label() == 'light_spline_MainRoad_R003_-0540_S05'), None)
    if target:
        eas.set_selected_level_actors([target])
    unreal.log('LIGHT_SPLINE_DONE: %d 条，文件夹 light_spline，红色，已保存' % len(planned))


if __name__ == '__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
