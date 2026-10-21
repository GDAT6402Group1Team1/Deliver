# -*- coding: utf-8 -*-
"""py trim_close_light_spline_ends.py：相邻红线端点太近时，各向内删4点。"""
import datetime
import json
import math
import traceback

import unreal

from cut_light_splines_at_roads import backup_actor, write_points
from trim_crossing_light_splines import DONE_TAG as NO_CROSS_TAG, crossings, live_segments

FOLDER = 'light_spline'
PREFIX = 'light_spline_'
END_GAP_CM = 600.0  # 两条红线端点的水平距离小于等于此值，就各删靠近端4点。
EXAMPLE_PAIR = (
    'light_spline_MainRoad_R010_-0540_S05',
    'light_spline_MainRoad_R009_-0540_S00',
)
EXAMPLE_MAX_GAP_CM = 1000.0  # 示例这对允许用更远的搜索范围选最近两端。
HEAD_TAG = 'DeliveryLightSplineCloseEndHeadTrim4V2'
TAIL_TAG = 'DeliveryLightSplineCloseEndTailTrim4V2'
WS = unreal.SplineCoordinateSpace.WORLD
ROOT = unreal.Paths.project_saved_dir()


def distance_xy(a, b):
    return math.hypot(a.x-b.x, a.y-b.y)


def endpoint(row, end):
    return row['points'][0 if end == 'head' else -1]


def candidate_pairs(rows):
    pairs = []
    example = frozenset(EXAMPLE_PAIR)
    for left_index, left in enumerate(rows):
        for right_index in range(left_index+1, len(rows)):
            right = rows[right_index]
            options = []
            for left_end in ('head', 'tail'):
                for right_end in ('head', 'tail'):
                    gap = distance_xy(endpoint(left, left_end), endpoint(right, right_end))
                    if gap <= END_GAP_CM:
                        options.append((gap, left_end, right_end))
            is_example = frozenset((left['label'], right['label'])) == example
            if is_example and not options:
                nearest = min(
                    (distance_xy(endpoint(left, le), endpoint(right, re)), le, re)
                    for le in ('head', 'tail') for re in ('head', 'tail')
                )
                if nearest[0] <= EXAMPLE_MAX_GAP_CM:
                    options.append(nearest)
            for gap, left_end, right_end in options:
                pairs.append(dict(left=left_index, left_end=left_end,
                                  right=right_index, right_end=right_end,
                                  gap_cm=round(gap, 1), example=is_example))
    return pairs


def main():
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate；未修改关卡')
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    rows = []
    for actor in eas.get_all_level_actors():
        label = actor.get_actor_label()
        if not label.startswith(PREFIX) or str(actor.get_folder_path()).split('/')[0] != FOLDER:
            continue
        spline = actor.get_component_by_class(unreal.SplineComponent)
        if not spline or spline.is_closed_loop():
            raise RuntimeError(label + ' 缺少样条或为闭合线；未修改关卡')
        count = spline.get_number_of_spline_points()
        if count < 2:
            raise RuntimeError(label + ' 少于2个点；未修改关卡')
        tags = [str(tag) for tag in actor.tags]
        if NO_CROSS_TAG not in tags:
            raise RuntimeError(label + ' 尚未完成交叉线清理；请先运行 trim_crossing_light_splines.py')
        for index in range(count):
            if spline.get_spline_point_type(index) != unreal.SplinePointType.LINEAR:
                raise RuntimeError(label + ' 不是折线；未修改关卡')
        rows.append(dict(actor=actor, spline=spline, label=label, tags=tags,
                         points=[spline.get_location_at_spline_point(i, WS) for i in range(count)]))
    rows.sort(key=lambda row: row['label'])
    if not rows:
        raise RuntimeError('没有找到 light_spline 文件夹中的红线')

    pairs = candidate_pairs(rows)
    marked = set()
    for pair in pairs:
        for row_index, end in ((pair['left'], pair['left_end']),
                               (pair['right'], pair['right_end'])):
            tag = HEAD_TAG if end == 'head' else TAIL_TAG
            if tag not in rows[row_index]['tags']:
                marked.add((row_index, end))

    plans = []
    for row_index, row in enumerate(rows):
        head = (row_index, 'head') in marked
        tail = (row_index, 'tail') in marked
        if not (head or tail):
            row['after'] = row['points']
            continue
        start = 4 if head else 0
        stop = len(row['points'])-4 if tail else len(row['points'])
        row['after'] = row['points'][start:stop] if stop-start >= 2 else []
        row['trim_head'] = head
        row['trim_tail'] = tail
        plans.append(row)

    # 删端点只会减少折线段；保存前仍完整复核红线间没有相交。
    geometry = [dict(points=row['after'], removed=set()) for row in rows]
    remaining_crossings = crossings(live_segments(geometry))
    if remaining_crossings:
        raise RuntimeError('仍检测到%d处红线相交；未修改关卡' % len(remaining_crossings))

    report = dict(endpoint_threshold_cm=END_GAP_CM,
                  example_max_gap_cm=EXAMPLE_MAX_GAP_CM,
                  red_lines=len(rows), close_endpoint_pairs=len(pairs),
                  changed=len(plans), removed_actors=sum(not row['after'] for row in plans),
                  remaining_crossings=0,
                  pairs=[dict(left=rows[p['left']]['label'], left_end=p['left_end'],
                              right=rows[p['right']]['label'], right_end=p['right_end'],
                              gap_cm=p['gap_cm'], example=p['example']) for p in pairs],
                  changes=[dict(label=row['label'], points_before=len(row['points']),
                                points_after=len(row['after']),
                                trim_head=row['trim_head'], trim_tail=row['trim_tail'])
                           for row in plans])
    example_found = any(pair['example'] for pair in pairs)
    if not example_found:
        matching = [row for row in rows if row['label'] in EXAMPLE_PAIR]
        report['example_note'] = ('示例线未全部找到' if len(matching) != 2 else
                                  '示例两条线最近端点超过 %.0f cm，未强制删点' % EXAMPLE_MAX_GAP_CM)

    if plans:
        stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
        backup = ROOT + 'light_spline_close_ends_backup_' + stamp + '.json'
        with open(backup, 'w', encoding='utf-8') as output:
            json.dump([backup_actor(row['actor'], row['spline']) for row in plans],
                      output, ensure_ascii=False, indent=2)
        report['backup'] = backup
        with unreal.ScopedEditorTransaction('红线相邻端点各删4点'):
            for row in plans:
                if not row['after']:
                    if not eas.destroy_actor(row['actor']):
                        raise RuntimeError('删除过短红线失败: ' + row['label'])
                    continue
                write_points(row['actor'], row['after'])
                tags = list(row['tags'])
                if row['trim_head']:
                    tags.append(HEAD_TAG)
                if row['trim_tail']:
                    tags.append(TAIL_TAG)
                row['actor'].tags = list(dict.fromkeys(tags))
        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('地图保存失败；备份在 ' + backup)

    path = ROOT + 'light_spline_close_ends_report.json'
    with open(path, 'w', encoding='utf-8') as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
    unreal.log('LIGHT_SPLINE_CLOSE_ENDS_DONE: 近距离端点对%d，修改%d条，删除过短线%d条；报告 %s' %
               (len(pairs), len(plans), report['removed_actors'], path))
    if 'example_note' in report:
        unreal.log_warning(report['example_note'])
    return report


if __name__ == '__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
