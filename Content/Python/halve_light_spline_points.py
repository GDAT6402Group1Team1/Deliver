# -*- coding: utf-8 -*-
"""py halve_light_spline_points.py：红线隔点删点，必要处保留点以避开道路和相交。"""
import datetime
import json
import traceback

import unreal

from cut_light_splines_at_roads import RoadQuery, backup_actor, write_points
from trim_crossing_light_splines import (
    DONE_TAG as NO_CROSS_TAG, crossings, live_segments, on_road_segments,
)

FOLDER = 'light_spline'
PREFIX = 'light_spline_'
DONE_TAG = 'DeliveryLightSplineHalfDensityV1'
WS = unreal.SplineCoordinateSpace.WORLD
ROOT = unreal.Paths.project_saved_dir()


def segments_for(rows):
    geometry = []
    for row in rows:
        row['indices'] = sorted(row['keep'])
        geometry.append(dict(points=[row['points'][i] for i in row['indices']], removed=set()))
    return live_segments(geometry)


def restore_midpoint(rows, segment):
    row_index, coarse_index = segment[:2]
    indices = rows[row_index]['indices']
    left, right = indices[coarse_index:coarse_index+2]
    if right-left <= 1:
        return False
    middle = (left+right)//2
    if middle in rows[row_index]['keep']:
        return False
    rows[row_index]['keep'].add(middle)
    return True


def main():
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate；未修改关卡')
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = list(eas.get_all_level_actors())
    rows = []
    for actor in actors:
        label = actor.get_actor_label()
        if not label.startswith(PREFIX) or str(actor.get_folder_path()).split('/')[0] != FOLDER:
            continue
        spline = actor.get_component_by_class(unreal.SplineComponent)
        if not spline or spline.is_closed_loop():
            raise RuntimeError(label + ' 缺少样条或为闭合线；未修改关卡')
        count = spline.get_number_of_spline_points()
        if count < 2:
            raise RuntimeError(label + ' 少于2点；未修改关卡')
        tags = [str(tag) for tag in actor.tags]
        if NO_CROSS_TAG not in tags:
            raise RuntimeError(label + ' 未运行交叉清理；请先运行 trim_crossing_light_splines.py')
        for index in range(count):
            if spline.get_spline_point_type(index) != unreal.SplinePointType.LINEAR:
                raise RuntimeError(label + ' 不是折线；未修改关卡')
        points = [spline.get_location_at_spline_point(i, WS) for i in range(count)]
        keep = set(range(count)) if DONE_TAG in tags else set(range(0, count, 2)) | {count-1}
        rows.append(dict(actor=actor, spline=spline, label=label, tags=tags,
                         points=points, keep=keep))
    if not rows:
        raise RuntimeError('没有找到 light_spline 文件夹中的红线')
    rows.sort(key=lambda row: row['label'])
    road = RoadQuery(actors)

    # 先确认原始几何安全：后续只恢复删掉的旧点，最终总能退回原状。
    for row in rows:
        row['keep'] = set(range(len(row['points'])))
    original_segments = segments_for(rows)
    original_crossings = crossings(original_segments)
    original_road_hits = on_road_segments(original_segments, road)
    if original_crossings or original_road_hits:
        raise RuntimeError('原红线有%d处相交、%d段压路；未修改关卡，请先清理原线' %
                           (len(original_crossings), len(original_road_hits)))

    for row in rows:
        count = len(row['points'])
        row['keep'] = (set(range(count)) if DONE_TAG in row['tags'] else
                       set(range(0, count, 2)) | {count-1})
    restored_for_crossings = 0
    restored_for_road = 0
    for iteration in range(50):
        segments = segments_for(rows)
        cross_hits = crossings(segments)
        road_hits = on_road_segments(segments, road)
        if not cross_hits and not road_hits:
            break
        restored = 0
        for left, right, _ in cross_hits:
            for segment in (left, right):
                if restore_midpoint(rows, segment):
                    restored += 1
                    restored_for_crossings += 1
        for segment in road_hits:
            if restore_midpoint(rows, segment):
                restored += 1
                restored_for_road += 1
        if not restored:
            raise RuntimeError('局部恢复旧点后仍无法消除相交或压路；未修改关卡')
    else:
        raise RuntimeError('50轮仍未通过几何检查；未修改关卡')

    plans = [row for row in rows if len(row['keep']) < len(row['points'])]
    report = dict(red_lines=len(rows), changed=len(plans),
                  points_before=sum(len(row['points']) for row in rows),
                  points_after=sum(len(row['keep']) for row in rows),
                  restored_for_crossings=restored_for_crossings,
                  restored_for_road=restored_for_road,
                  remaining_crossings=0, remaining_road_overlaps=0,
                  rows=[dict(label=row['label'], before=len(row['points']),
                             after=len(row['keep']), removed=len(row['points'])-len(row['keep']))
                        for row in plans])
    if plans:
        stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
        backup = ROOT + 'light_spline_half_density_backup_' + stamp + '.json'
        with open(backup, 'w', encoding='utf-8') as output:
            json.dump([backup_actor(row['actor'], row['spline']) for row in plans],
                      output, ensure_ascii=False, indent=2)
        report['backup'] = backup
        with unreal.ScopedEditorTransaction('红线控制点减半'):
            for row in plans:
                points = [row['points'][i] for i in sorted(row['keep'])]
                write_points(row['actor'], points)
                row['actor'].tags = list(dict.fromkeys(row['tags'] + [DONE_TAG]))
        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('地图保存失败；备份在 ' + backup)

    path = ROOT + 'light_spline_half_density_report.json'
    with open(path, 'w', encoding='utf-8') as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
    unreal.log('LIGHT_SPLINE_HALF_DENSITY_DONE: %d条红线，%d条已减密，点数%d -> %d；报告 %s' %
               (len(rows), len(plans), report['points_before'], report['points_after'], path))
    return report


if __name__ == '__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
