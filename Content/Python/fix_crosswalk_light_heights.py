# -*- coding: utf-8 -*-
"""用斑马线网格靠近灯的一端的实际高度，抬起埋地的人行灯。"""
import statistics
import unreal

LEVEL = '/Game/Level/TestForCharacter'
TAG = 'ClaudeGenCrosswalkLight'
DRY_RUN = False
MIN_BURIED_CM = 10.0
SURFACE_LIFT_CM = 0.8

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not levels.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
e = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = e.get_all_level_actors()
marks = {a.get_actor_label(): a for a in actors
         if a.get_actor_label().startswith('Crosswalk_')}
lights = [a for a in actors if TAG in [str(t) for t in a.tags]]
if len(lights) != 48:
    raise RuntimeError('Expected 48 pedestrian lights, found %d; nothing changed' % len(lights))

cache = {}
def surface_vertices(mark):
    name = mark.get_actor_label()
    if name in cache:
        return cache[name]
    points = []
    for component in mark.get_components_by_class(unreal.StaticMeshComponent):
        if not component.static_mesh:
            continue
        mesh = unreal.DynamicMesh()
        _, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(
            component.static_mesh, mesh,
            unreal.GeometryScriptCopyMeshFromAssetOptions(),
            unreal.GeometryScriptMeshReadLOD())
        if outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
            raise RuntimeError('Cannot read marking mesh on ' + name)
        _, vertices, _ = unreal.GeometryScript_MeshQueries.get_all_vertex_positions(mesh, True)
        transform = component.get_world_transform()
        for index in range(vertices.get_vector_list_length()):
            point, valid = vertices.get_vector_list_item(index)
            if valid:
                points.append(transform.transform_location(point))
    if not points:
        raise RuntimeError('No marking vertices on ' + name)
    cache[name] = points
    return points

planned = []
for light in lights:
    label = light.get_actor_label()
    if not label.startswith('PedLight_') or not label.endswith(('_A', '_B')):
        raise RuntimeError('Unexpected light label ' + label)
    mark_name = 'Crosswalk_' + label[len('PedLight_'):-2]
    mark = marks.get(mark_name)
    if not mark:
        raise RuntimeError('Missing marking ' + mark_name)
    loc = light.get_actor_location()
    nearest = sorted(surface_vertices(mark),
                     key=lambda p: (p.x-loc.x)**2 + (p.y-loc.y)**2)[:8]
    road_z = statistics.median(p.z for p in nearest) - SURFACE_LIFT_CM
    buried = road_z - loc.z
    if buried > MIN_BURIED_CM:
        planned.append((light, unreal.Vector(loc.x, loc.y, road_z), buried))
        unreal.log('[CrosswalkHeight] %s: buried %.1f cm, Z %.1f -> %.1f' %
                   (label, buried, loc.z, road_z))

unreal.log('[CrosswalkHeight] checked %d lights, planned %d raises, DRY_RUN=%s' %
           (len(lights), len(planned), DRY_RUN))
if not DRY_RUN:
    for light, target, _ in planned:
        light.set_actor_location(target, False, False)
    if planned and not levels.save_current_level():
        raise RuntimeError('Could not save level')
