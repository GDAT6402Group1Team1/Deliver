# -*- coding: utf-8 -*-
"""在 Crosswalk_Intersection_45_02 摆小孩过街路线，只替换本脚本的路线。"""
import math
import re
import unreal

LEVEL = '/Game/Level/TestForCharacter'
MARK = 'Crosswalk_Intersection_45_02'
TAG = 'DeliveryChildCrosswalk4502'
LABEL = 'PedestrianRoute_ChildCrosswalk4502'
EXTEND = 250.0
WS = unreal.SplineCoordinateSpace.WORLD

level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not level.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
e = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = e.get_all_level_actors()
mark = next((a for a in actors if a.get_actor_label() == MARK), None)
if not mark:
    raise RuntimeError('Missing ' + MARK)
loc = mark.get_actor_location()
fv = mark.get_actor_forward_vector()
lat = (-fv.y, fv.x)
n = math.hypot(*lat)
lat = (lat[0]/n, lat[1]/n)
values = []
for mesh in mark.get_components_by_class(unreal.StaticMeshComponent):
    if not mesh.static_mesh:
        continue
    box = mesh.static_mesh.get_bounding_box()
    xf = mesh.get_world_transform()
    for x in (box.min.x, box.max.x):
        for y in (box.min.y, box.max.y):
            for z in (box.min.z, box.max.z):
                p = xf.transform_location(unreal.Vector(x,y,z))
                values.append(p.x*lat[0] + p.y*lat[1])
if not values:
    raise RuntimeError('Crosswalk has no mesh bounds')
base = loc.x*lat[0] + loc.y*lat[1]
points = []
for edge in (min(values)-EXTEND, max(values)+EXTEND):
    points.append(unreal.Vector(loc.x+lat[0]*(edge-base),
                                loc.y+lat[1]*(edge-base), loc.z))

# 和摆人行灯脚本相同的路口段归属判据；必须只匹配一个灯组
matches = []
for a in actors:
    if not re.match(r'^Inter_(.+)_([+-]\d+)_I\d+$', a.get_actor_label()):
        continue
    splines = a.get_components_by_class(unreal.SplineComponent)
    sp = next((x for x in splines if x.get_name() == 'Spline'), splines[0] if splines else None)
    if not sp or sp.get_spline_length() < 1:
        continue
    p0 = sp.get_location_at_distance_along_spline(0, WS)
    p1 = sp.get_location_at_distance_along_spline(min(200,sp.get_spline_length()), WS)
    dx,dy = p1.x-p0.x,p1.y-p0.y
    d = math.hypot(dx,dy)
    if d < 0.01:
        continue
    fx,fy = dx/d,dy/d
    rx,ry = loc.x-p0.x,loc.y-p0.y
    behind = -(rx*fx+ry*fy)
    lateral = abs(-rx*fy+ry*fx)
    if 150 <= behind <= 800 and lateral <= 1100:
        matches.append(a)
if not matches:
    raise RuntimeError('No signal segment for ' + MARK)
groups = {int(a.get_editor_property('LightNumber')) for a in matches}
if len(groups) != 1:
    raise RuntimeError('Ambiguous signal groups: ' + str(groups))
signal = matches[0]

kinds = []
for name in ('SK_NpcBoy','SK_NpcGirl'):
    mesh = unreal.load_asset('/Game/Characters/NPC/%s.%s' % (name,name))
    if not isinstance(mesh, unreal.SkeletalMesh):
        raise RuntimeError('Missing child mesh ' + name)
    kind = unreal.DeliveryPedestrianKind()
    kind.set_editor_property('mesh', mesh)
    kind.set_editor_property('weight', 1.0)
    kind.set_editor_property('height_scale', 0.60)
    kinds.append(kind)
route_class = getattr(unreal, 'DeliveryPedestrianRoute', None)
if route_class is None:
    raise RuntimeError('Build DeliveryEditor first')
old = [a for a in actors if TAG in [str(t) for t in a.tags]]
route = e.spawn_actor_from_class(route_class, points[0])
if not route:
    raise RuntimeError('Could not spawn route')
route.set_actor_label(LABEL)
route.set_folder_path('Pedestrians/Crosswalk')
route.tags = [unreal.Name(TAG)]
route.set_editor_property('kinds', kinds)
fade_materials = [unreal.load_asset('/Game/Characters/NPC/M_ChildBoy_Fade'),
                  unreal.load_asset('/Game/Characters/NPC/M_ChildGirl_Fade')]
if any(mat is None for mat in fade_materials):
    raise RuntimeError('Missing child fade materials; run create_child_fade_materials.py')
route.set_editor_property('fade_materials', fade_materials)
route.set_editor_property('fade_instead_of_scale', True)
route.set_editor_property('roam_in_area', False)
route.set_editor_property('both_directions', False)
route.set_editor_property('allow_handoff', False)
route.set_editor_property('prewarm_on_begin_play', False)
route.set_editor_property('pause_while_walking', False)
route.set_editor_property('max_alive', 12)
route.set_editor_property('spawn_interval_min', 1.25)
route.set_editor_property('spawn_interval_max', 1.25)
route.set_editor_property('walk_speed_min', 130.0)
route.set_editor_property('walk_speed_max', 130.0)
route.set_editor_property('lateral_jitter', 0.0)
route.set_editor_property('appear_distance', 100.0)
route.set_editor_property('min_visible_scale', 0.02)
route.set_editor_property('red_light_walk_speed_multiplier', 1.5)
route.set_editor_property('crosswalk_actor', mark)
route.set_editor_property('signal_segment', signal)
sp = route.get_component_by_class(unreal.SplineComponent)
sp.clear_spline_points(False)
for p in points:
    sp.add_spline_point(p, WS, False)
for i in range(2):
    sp.set_spline_point_type(i, unreal.SplinePointType.LINEAR, False)
sp.set_closed_loop(False, False)
sp.update_spline()
for a in old:
    e.destroy_actor(a)
if not level.save_current_level():
    raise RuntimeError('Could not save level')
unreal.log('[ChildCrosswalk] placed %s, length %.0f cm, signal %s' %
           (LABEL, sp.get_spline_length(), signal.get_actor_label()))
