# -*- coding: utf-8 -*-
"""py trim_light_splines.py：现有 light_spline 头尾各删2点；重复运行不重复裁剪。"""
import json
import traceback
import unreal

TAG = 'DeliveryLightSplineTrim2EachEnd'
WS = unreal.SplineCoordinateSpace.WORLD


def main():
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('请先停止 PIE / Simulate')
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    changed, skipped = [], []
    with unreal.ScopedEditorTransaction('light_spline 头尾各删两个点'):
        for a in eas.get_all_level_actors():
            if not a.get_actor_label().startswith('light_spline_'):
                continue
            if str(a.get_folder_path()).split('/')[0] != 'light_spline':
                continue
            tags = [str(t) for t in a.tags]
            s = a.get_component_by_class(unreal.SplineComponent)
            if not s:
                continue
            n = s.get_number_of_spline_points()
            if TAG in tags or n < 6 or s.is_closed_loop():
                reason = '已经裁剪' if TAG in tags else '少于6点' if n < 6 else '闭合样条'
                skipped.append({'label': a.get_actor_label(), 'reason':reason})
                continue
            expected = [s.get_location_at_spline_point(i, WS) for i in range(2,n-2)]
            a.modify(True)
            s.set_editor_property('spline_has_been_edited', True)
            s = a.get_component_by_class(unreal.SplineComponent)
            s.modify(True)
            # 倒序删，避免索引移动；中间点的类型、旋转和缩放沿用原值。
            for index in (n-1, n-2, 1, 0):
                s.remove_spline_point(index, False)
            for index in (0, n-5):
                s.set_spline_point_type(index, unreal.SplinePointType.CURVE, False)
            s.update_spline()
            assert s.get_number_of_spline_points() == n-4, a.get_actor_label()
            for i,p in enumerate(expected):
                assert (s.get_location_at_spline_point(i, WS)-p).length() < 0.1, a.get_actor_label()
            a.tags = tags + [TAG]
            changed.append({'label':a.get_actor_label(), 'before':n, 'after':n-4})
    if changed:
        if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
            raise RuntimeError('保存失败')
    with open(unreal.Paths.project_saved_dir()+'trim_light_splines.json','w',encoding='utf-8') as f:
        json.dump({'changed':changed,'skipped':skipped},f,ensure_ascii=False,indent=2)
    unreal.log('LIGHT_SPLINE_TRIM_DONE: 修改 %d 条，跳过 %d 条，头尾各删2点' % (len(changed),len(skipped)))


if __name__ == '__main__':
    try:
        main()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
