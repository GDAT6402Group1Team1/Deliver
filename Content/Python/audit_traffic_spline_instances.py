# -*- coding: utf-8 -*-
"""py audit_traffic_spline_instances.py：只读核对当前关卡的 Lane_ / Inter_ 实例类型。"""
import json
import traceback

import unreal

LANE_BP = '/Game/PS2DEM/BP_TrafficLine1'
INTER_BP = '/Game/PS2DEM/BP_TrafficLine1_IntersectionChild'


def load_class(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if not asset or not asset.generated_class():
        raise RuntimeError('蓝图未找到或未编译: ' + path)
    return asset.generated_class()


def classify(actor, lane_class, inter_class):
    label = actor.get_actor_label()
    if label.startswith('Lane_'):
        kind = 'Lane'
    elif label.startswith('Inter_'):
        kind = 'Inter'
    else:
        return None

    actor_class = actor.get_class()
    is_lane_class = unreal.MathLibrary.class_is_child_of(actor_class, lane_class)
    is_inter_class = unreal.MathLibrary.class_is_child_of(actor_class, inter_class)
    has_spline = bool(actor.get_component_by_class(unreal.SplineComponent))
    if kind == 'Lane':
        valid = is_lane_class and not is_inter_class and has_spline
        expected = 'BP_TrafficLine1（不能是 IntersectionChild）'
    else:
        valid = is_inter_class and has_spline
        expected = 'BP_TrafficLine1_IntersectionChild 或其子类'
    return dict(label=label, kind=kind, actor_class=actor_class.get_path_name(),
                folder=str(actor.get_folder_path()), has_spline=has_spline,
                is_lane_class=is_lane_class, is_inter_class=is_inter_class,
                expected=expected, valid=valid)


def audit(actors):
    lane_class = load_class(LANE_BP)
    inter_class = load_class(INTER_BP)
    records = [record for actor in actors
               for record in [classify(actor, lane_class, inter_class)] if record]
    records.sort(key=lambda item: item['label'])
    return dict(lane_count=sum(item['kind'] == 'Lane' for item in records),
                inter_count=sum(item['kind'] == 'Inter' for item in records),
                lane_valid=sum(item['kind'] == 'Lane' and item['valid'] for item in records),
                inter_valid=sum(item['kind'] == 'Inter' and item['valid'] for item in records),
                anomalies=[item for item in records if not item['valid']],
                records=records)


def main():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    report = audit(eas.get_all_level_actors())
    path = unreal.Paths.project_saved_dir() + 'traffic_spline_instance_audit.json'
    with open(path, 'w', encoding='utf-8') as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
    unreal.log('TRAFFIC_SPLINE_AUDIT: Lane %d/%d 类型正确，Inter %d/%d 类型正确，异常 %d；报告 %s' %
               (report['lane_valid'], report['lane_count'],
                report['inter_valid'], report['inter_count'],
                len(report['anomalies']), path))
    for item in report['anomalies']:
        unreal.log_warning('%s: 实际类型 %s，预期 %s' %
                           (item['label'], item['actor_class'], item['expected']))
    return report


if __name__ == '__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
