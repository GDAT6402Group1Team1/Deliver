# -*- coding: utf-8 -*-
"""py organize_traffic_splines.py：将当前关卡的 Lane_ / Inter_ 样条实例归入同一文件夹。"""
import datetime
import json
import traceback

import unreal
from audit_traffic_spline_instances import audit

FOLDER = 'TrafficSplines'
PREFIXES = ('Lane_', 'Inter_')


def main():
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate；未修改关卡')

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = list(eas.get_all_level_actors())
    verification = audit(actors)
    if verification['anomalies']:
        labels = ', '.join(item['label'] for item in verification['anomalies'][:8])
        raise RuntimeError('发现%d个 Lane_/Inter_ 实例类型不符，未移动任何对象：%s。请先运行 audit_traffic_spline_instances.py' %
                           (len(verification['anomalies']), labels))
    matched = []
    for actor in actors:
        label = actor.get_actor_label()
        if not label.startswith(PREFIXES):
            continue
        matched.append((actor, label, str(actor.get_folder_path())))

    if not matched:
        raise RuntimeError('当前关卡没有找到 Lane_ / Inter_ 样条实例；未修改关卡')

    changes = [(actor, label, old_folder) for actor, label, old_folder in matched
               if old_folder != FOLDER]
    report = {
        'target_folder': FOLDER,
        'matched': len(matched),
        'moved': len(changes),
        'already_in_folder': len(matched) - len(changes),
        'lane_verified': verification['lane_valid'],
        'inter_verified': verification['inter_valid'],
        'changes': [{'label': label, 'old_folder': old_folder, 'new_folder': FOLDER}
                    for _, label, old_folder in changes],
    }

    if changes:
        stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
        backup_path = unreal.Paths.project_saved_dir() + 'traffic_spline_folders_backup_' + stamp + '.json'
        with open(backup_path, 'w', encoding='utf-8') as output:
            json.dump(report, output, ensure_ascii=False, indent=2)
        report['backup'] = backup_path

        with unreal.ScopedEditorTransaction('整理交通和路口样条文件夹'):
            for actor, _, _ in changes:
                actor.modify(True)
                actor.set_folder_path(FOLDER)

        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('关卡保存失败；原文件夹记录在 ' + backup_path)

    report_path = unreal.Paths.project_saved_dir() + 'traffic_spline_folders_report.json'
    with open(report_path, 'w', encoding='utf-8') as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
    unreal.log('TRAFFIC_SPLINES_FOLDER_DONE: 找到%d条，移动%d条，已在文件夹中%d条；报告 %s' %
               (len(matched), len(changes), len(matched)-len(changes), report_path))
    return report


if __name__ == '__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
