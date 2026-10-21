# -*- coding: utf-8 -*-
"""只读核对保存后的路灯路径。命令行验证时显式传 --load-light-map。"""
import json
import sys
import unreal

if '--load-light-map' in sys.argv:
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level('/Game/Level/TestForCharacter')
root = unreal.Paths.project_saved_dir()
with open(root + 'light_splines.json', encoding='utf-8') as f:
    expected = json.load(f)
actors = list(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors())
generated = [a for a in actors if 'DeliveryLightSpline' in [str(t) for t in a.tags]]
assert len(generated) == len(expected['splines']), (len(generated), len(expected['splines']))
lookup = {a.get_actor_label(): a for a in generated}
assert len(lookup) == len(generated), '重名'
errors = []
for row in expected['splines']:
    a = lookup.get(row['label'])
    if not a:
        errors.append(row['label'] + ': missing')
        continue
    s = a.get_component_by_class(unreal.SplineComponent)
    if not s or s.get_number_of_spline_points() != row['points']:
        errors.append(row['label'] + ': points mismatch')
        continue
    assert str(a.get_folder_path()) == 'light_spline'
    for prop in ('editor_unselected_spline_segment_color', 'editor_selected_spline_segment_color'):
        c = s.get_editor_property(prop)
        assert c.r == 1.0 and c.g == 0.0 and c.b == 0.0, row['label'] + ': color'
    for index, name in ((0, 'first'), (row['points']-1, 'last')):
        p = s.get_location_at_spline_point(index, unreal.SplineCoordinateSpace.WORLD)
        assert max(abs(x-y) for x,y in zip((p.x,p.y,p.z),row[name])) < 0.1, row['label'] + ': position'
assert not errors, errors
result = {'verified':len(generated), 'total_points':sum(r['points'] for r in expected['splines']),
          'errors':errors, 'map':unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world().get_path_name()}
with open(root + 'light_splines_verified.json','w',encoding='utf-8') as f:
    json.dump(result,f,indent=2)
unreal.log('LIGHT_SPLINE_VERIFIED ' + str(result))
