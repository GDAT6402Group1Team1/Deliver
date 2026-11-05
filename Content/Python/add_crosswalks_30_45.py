# -*- coding: utf-8 -*-
"""Add full-road surface-painted crosswalks at Intersection_30 and _45.

Run in the open TestForCharacter editor: py add_crosswalks_30_45.py
For a read-only geometry check: import add_crosswalks_30_45 as c; c.run(False)
Existing Crosswalk_* actors are never changed. Traffic splines and transforms are
read-only; only the new entrance segments receive the Crosswalk stop-line tag.
"""
import json
import math
import re
import traceback
from pathlib import Path

import unreal
import setup_crosswalks as surface

TARGETS = ('Intersection_30', 'Intersection_45')
INTER_NAME = re.compile(r'^Inter_(.+)_([+-]\d+)_I\d+$')
BOX_HALF = 128.0
SETBACK = 400.0
SAME_DIR_DOT = .8
GROUP_RADIUS = 700.0
REPORT = Path(unreal.Paths.project_saved_dir()) / 'crosswalk_30_45.json'
SPACE = unreal.SplineCoordinateSpace.WORLD


def spline_entry(actor):
    sp = actor.get_component_by_class(unreal.SplineComponent)
    if not sp or sp.get_number_of_spline_points() < 2:
        return None
    length = sp.get_spline_length()
    if length <= 1:
        return None
    p0 = sp.get_location_at_distance_along_spline(0.0, SPACE)
    p1 = sp.get_location_at_distance_along_spline(min(200.0, length), SPACE)
    dx, dy = p1.x-p0.x, p1.y-p0.y
    n = math.hypot(dx, dy)
    return (p0, unreal.Vector(dx/n, dy/n, 0)) if n > .01 else None


def inside_box(point, box):
    center, extent, yaw = box
    dx, dy = point.x-center.x, point.y-center.y
    c, s = math.cos(math.radians(-yaw)), math.sin(math.radians(-yaw))
    return abs(dx*c-dy*s) <= extent.x and abs(dx*s+dy*c) <= extent.y


class Anchor:
    def __init__(self, label, origin, forward):
        self.label, self.origin, self.forward = label, origin, forward

    def get_actor_label(self):
        return self.label

    def get_actor_location(self):
        return self.origin

    def get_actor_forward_vector(self):
        return self.forward

    def get_component_by_class(self, cls):
        return None


def candidates(actors):
    junctions = {a.get_actor_label(): a for a in actors if a.get_actor_label() in TARGETS}
    if set(junctions) != set(TARGETS):
        raise RuntimeError('Both Intersection_30 and Intersection_45 must exist')
    segments = []
    for a in actors:
        m = INTER_NAME.match(a.get_actor_label())
        if not m:
            continue
        entry = spline_entry(a)
        if entry:
            segments.append(dict(actor=a, road=m.group(1), p0=entry[0], direction=entry[1]))

    result = []
    for name in TARGETS:
        j = junctions[name]
        boxes = j.get_components_by_class(unreal.BoxComponent)
        if not boxes:
            raise RuntimeError(name + ' has no Box')
        b = boxes[0]
        box = (b.get_world_location(), b.get_scaled_box_extent(), b.get_world_rotation().yaw)
        owned = [s for s in segments if inside_box(s['p0'], box)]
        groups = []
        for s in owned:
            p0, d = s['p0'], s['direction']
            center = unreal.Vector(p0.x-d.x*(BOX_HALF+SETBACK/2),
                                   p0.y-d.y*(BOX_HALF+SETBACK/2), p0.z)
            group = next((g for g in groups if g['road'] == s['road']
                          and g['direction'].x*d.x+g['direction'].y*d.y >= SAME_DIR_DOT
                          and math.hypot(g['center'].x-center.x,g['center'].y-center.y)
                          <= GROUP_RADIUS), None)
            if group is None:
                groups.append(dict(road=s['road'], direction=d, center=center, members=[s]))
            else:
                group['members'].append(s)
                n = len(group['members'])
                group['center'] = unreal.Vector(group['center'].x+(center.x-group['center'].x)/n,
                                                 group['center'].y+(center.y-group['center'].y)/n,
                                                 group['center'].z+(center.z-group['center'].z)/n)
        # Four approaches per crossing. Their road widths are measured by
        # setup_crosswalks.prepare, not inferred from the number of traffic lanes.
        if len(groups) != 4:
            raise RuntimeError('%s: expected four road approaches, found %d' % (name,len(groups)))
        groups.sort(key=lambda g: math.atan2(g['center'].y-j.get_actor_location().y,
                                              g['center'].x-j.get_actor_location().x))
        for index, g in enumerate(groups, 1):
            label = 'Crosswalk_%s_%02d' % (name, index)
            result.append(dict(label=label, junction=name, group=g,
                               anchor=Anchor(label,g['center'],g['direction'])))
    return result


def run(apply=True):
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if editor.get_game_world():
        raise RuntimeError('Stop PIE first')
    world = editor.get_editor_world()
    if not world or world.get_path_name() != '/Game/Level/TestForCharacter.TestForCharacter':
        raise RuntimeError('Open TestForCharacter first; this script never changes maps')
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = [a for a in eas.get_all_level_actors() if a]
    before = surface.traffic_snapshot(actors)
    found = candidates(actors)
    labels = {a.get_actor_label() for a in actors}
    new = [item for item in found if item['label'] not in labels]
    sampler = surface.RoadSampler(world, actors)
    plans = [surface.prepare(item['anchor'], sampler) for item in new]
    report = dict(status='measured',target_count=len(found),new_count=len(new),
                  crossings=[dict(p['info'], junction=item['junction'],
                                  segments=[s['actor'].get_actor_label() for s in item['group']['members']])
                             for item,p in zip(new,plans)])
    REPORT.write_text(json.dumps(report,indent=2),encoding='utf-8')
    if not apply or not new:
        return report
    material = unreal.load_asset(surface.MATERIAL)
    if not material:
        raise RuntimeError('Missing existing crosswalk paint material')
    meshes = [surface.build_mesh(p,material) for p in plans]
    created=[]
    with unreal.ScopedEditorTransaction('Add crosswalks at intersections 30 and 45'):
        try:
            for item,p,mesh in zip(new,plans,meshes):
                rot=unreal.MathLibrary.make_rot_from_xz(p['forward'],unreal.Vector(0,0,1))
                a=eas.spawn_actor_from_class(unreal.StaticMeshActor,p['origin'],rot)
                if not a:
                    raise RuntimeError('Cannot spawn '+item['label'])
                created.append(a)
                a.modify(True)
                a.set_actor_label(item['label'])
                a.set_folder_path('Crosswalks')
                a.tags=[unreal.Name(surface.SURFACE_TAG)]
                c=a.static_mesh_component
                c.modify(True)
                c.set_static_mesh(mesh)
                c.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
                c.set_editor_property('cast_shadow',False)
                c.set_editor_property('affect_distance_field_lighting',False)
                a.set_actor_enable_collision(False)
            for item in new:
                for s in item['group']['members']:
                    a=s['actor']
                    tags=[str(t) for t in a.tags]
                    if 'Crosswalk' not in tags:
                        a.modify(True)
                        a.tags=[unreal.Name(t) for t in tags+['Crosswalk']]
        except Exception:
            for a in created:
                eas.destroy_actor(a)
            raise
    after=surface.traffic_snapshot(eas.get_all_level_actors())
    allowed={s['actor'].get_path_name() for item in new for s in item['group']['members']}
    for path,values in before.items():
        new_values=after[path]
        if path in allowed:
            assert values[:3]==new_values[:3] and values[4]==new_values[4], 'Traffic geometry changed'
            assert new_values[3]==list(dict.fromkeys(values[3]+['Crosswalk'])), 'Unexpected traffic tags'
        else:
            assert values==new_values, 'Unrelated traffic changed: '+path
    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError('Level save failed')
    report.update(status='saved',added_count=len(created),traffic_geometry_unchanged=True)
    REPORT.write_text(json.dumps(report,indent=2),encoding='utf-8')
    return report


if __name__=='__main__':
    try:
        run()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
