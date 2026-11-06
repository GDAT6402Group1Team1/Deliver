# -*- coding: utf-8 -*-
"""Rebuild existing Crosswalk_* markings as road-conforming white stripe meshes.

Run in the open editor: py setup_crosswalks.py
The existing crossing anchors/directions are authoritative: no junction selection,
traffic spline edits, tag edits on traffic actors, or automatic map switching.
The road is measured on BOTH sides of each existing crossing. All geometry is
prepared before any old decal is removed. A failed trace aborts, never guesses Z.
Saved static meshes work in packaged builds without Python or runtime raycasts.
"""
import json
import math
import traceback
from pathlib import Path

import unreal

LEGACY_TAG = 'ClaudeGenCrosswalk'
SURFACE_TAG = 'DeliveryCrosswalkSurfaceV2'
MATERIAL = '/Game/materials/M_CrosswalkPaint'
MESH_DIR = '/Game/Environment/RoadMarkings'
LENGTH = 400.0
STRIPE_WIDTH = 50.0
STRIPE_GAP = 50.0
EDGE_MARGIN = 20.0
SURFACE_LIFT = 0.8  # cm; enough depth separation, below a visible raised slab.
SAMPLE_STEP = 25.0
MAX_HALF_SCAN = 3000.0
REPORT = Path(unreal.Paths.project_saved_dir()) / 'crosswalk_rebuild.json'


def xyz(v):
    return [v.x, v.y, v.z]


def traffic_snapshot(actors):
    """Ensure this visual replacement cannot alter traffic or its stop-line tags."""
    result = {}
    for a in actors:
        if not a or not a.get_actor_label().startswith(('Inter_', 'Lane_', 'Intersection_')):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        points = []
        if sp:
            for i in range(sp.get_number_of_spline_points()):
                points.append(xyz(sp.get_location_at_spline_point(i, unreal.SplineCoordinateSpace.WORLD)))
        rot = a.get_actor_rotation()
        result[a.get_path_name()] = [xyz(a.get_actor_location()),
            [rot.pitch, rot.yaw, rot.roll], xyz(a.get_actor_scale3d()),
            [str(t) for t in a.tags], points]
    return result


class RoadSampler:
    def __init__(self, world, actors):
        self.world = world
        # Trace through scenery/vehicles, but never through a valid road surface.
        self.ignore = [a for a in actors if a and (a.get_actor_label().startswith('Crosswalk_')
                       or a.get_class().get_name().startswith('BP_car'))]
        self.cache = {}

    def hit(self, x, y, hint):
        key = (round(x, 4), round(y, 4), round(hint, 2))
        if key in self.cache:
            return self.cache[key]
        top, bottom = hint + 500.0, hint - 1200.0
        result = None
        for _ in range(16):
            h = unreal.SystemLibrary.line_trace_single(
                self.world, unreal.Vector(x, y, top), unreal.Vector(x, y, bottom),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, True, self.ignore,
                unreal.DrawDebugTrace.NONE, True)
            if not h:
                break
            d = h.to_dict()
            if not d.get('blocking_hit'):
                break
            p, c = d['impact_point'], d['hit_component']
            if isinstance(c, unreal.SplineMeshComponent) and c.static_mesh:
                result = (p.z, c.static_mesh.get_path_name(), d['impact_normal'])
                break
            top = min(top - 1.0, p.z - 1.0)
            if top <= bottom:
                break
        self.cache[key] = result
        return result


def prepare(actor, sampler):
    label = actor.get_actor_label()
    origin = actor.get_actor_location()
    old_decal = actor.get_component_by_class(unreal.DecalComponent)
    forward = actor.get_actor_up_vector() if old_decal else actor.get_actor_forward_vector()
    size = math.hypot(forward.x, forward.y)
    if size < 0.99:
        raise RuntimeError(label + ': crossing direction must be horizontal')
    forward = unreal.Vector(forward.x / size, forward.y / size, 0.0)
    right = unreal.Vector(-forward.y, forward.x, 0.0)
    base = sampler.hit(origin.x, origin.y, origin.z)
    if base is None:
        raise RuntimeError(label + ': no real road below the existing anchor')
    road_asset = base[1]

    def sample(u, v):
        h = sampler.hit(origin.x + right.x*u + forward.x*v,
                        origin.y + right.y*u + forward.y*v, origin.z)
        return h if h and h[1] == road_asset and h[2].z >= 0.7 else None

    def edge(sign, v):
        if sample(0, v) is None:
            raise RuntimeError(label + ': crossing extends off road at its anchor')
        inside = 0.0
        for step in range(1, int(MAX_HALF_SCAN / SAMPLE_STEP) + 1):
            outside = sign * step * SAMPLE_STEP
            if sample(outside, v) is None:
                # First loss of contiguous pavement, not some other road beyond it.
                for _ in range(7):
                    mid = (inside + outside) * 0.5
                    if sample(mid, v):
                        inside = mid
                    else:
                        outside = mid
                return inside
            inside = outside
        # Near the junction this ray can continue along the crossing street.
        # It contributes no usable edge; other sections must resolve both sides.
        return None

    # Intersection pavement can widen toward the junction. Use the common span
    # across the entire 4m marking so every white bar remains on the road.
    sections = [(edge(-1, v), edge(1, v)) for v in
                (-LENGTH/2, -LENGTH/4, 0.0, LENGTH/4, LENGTH/2)]
    left_edges = [a for a, b in sections if a is not None]
    right_edges = [b for a, b in sections if b is not None]
    if min(len(left_edges), len(right_edges)) < 2:
        raise RuntimeError(label + ': insufficient independent road-edge measurements')
    left = max(left_edges) + EDGE_MARGIN
    right_edge = min(right_edges) - EDGE_MARGIN
    span = right_edge - left
    if not 500.0 <= span <= 2600.0:
        raise RuntimeError(label + ': implausible pavement span %.1f' % span)
    count = max(2, round((span + STRIPE_GAP) / (STRIPE_WIDTH + STRIPE_GAP)))
    # Equal white/gap widths, both ends end on a complete white stripe.
    width = span / (count + (count - 1) * STRIPE_GAP / STRIPE_WIDTH)
    gap = width * STRIPE_GAP / STRIPE_WIDTH
    vertices, triangles, normals, uvs = [], [], [], []
    z_values = []
    for stripe in range(count):
        u0 = left + stripe * (width + gap)
        nx, ny = math.ceil(width / SAMPLE_STEP), math.ceil(LENGTH / SAMPLE_STEP)
        first = len(vertices)
        for iy in range(ny + 1):
            v = -LENGTH/2 + LENGTH * iy/ny
            for ix in range(nx + 1):
                u = u0 + width * ix/nx
                h = sample(u, v)
                if h is None:
                    raise RuntimeError(label + ': stripe vertex has no road support')
                vertices.append(unreal.Vector(v, u, h[0] - origin.z + SURFACE_LIFT))
                n = h[2]
                normals.append(unreal.Vector(n.x*forward.x+n.y*forward.y,
                                             n.x*right.x+n.y*right.y, n.z))
                uvs.append(unreal.Vector2D(ix/nx, iy/ny))
                z_values.append(h[0])
        for iy in range(ny):
            for ix in range(nx):
                a = first + iy*(nx+1) + ix
                b, c, d = a+1, a+nx+1, a+nx+2
                # Local X is along traffic; local Y crosses the road. +Z winding.
                triangles.extend([unreal.IntVector(a,b,c), unreal.IntVector(b,d,c)])
    return dict(actor=actor, origin=origin, forward=forward,
                vertices=vertices, triangles=triangles, normals=normals, uvs=uvs,
                info=dict(label=label, origin=xyz(origin), direction=xyz(forward),
                          span_cm=span, stripe_count=count, stripe_width_cm=width,
                          gap_cm=gap, length_cm=LENGTH, triangles=len(triangles),
                          road_height_min=min(z_values), road_height_max=max(z_values),
                          road_asset=road_asset, road_edges=sections,
                          surface_lift_cm=SURFACE_LIFT))


def paint_material():
    mat = unreal.load_asset(MATERIAL)
    if not mat:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            MATERIAL.rsplit('/',1)[1], MATERIAL.rsplit('/',1)[0],
            unreal.Material, unreal.MaterialFactoryNew())
        if not mat:
            raise RuntimeError('Cannot create crosswalk paint material')
    mat.modify(True)
    lib = unreal.MaterialEditingLibrary
    lib.delete_all_material_expressions(mat)
    mat.set_editor_property('material_domain', unreal.MaterialDomain.MD_SURFACE)
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property('two_sided', False)
    white = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -240, 0)
    white.set_editor_property('constant', unreal.LinearColor(0.86,0.86,0.84,1.0))
    lib.connect_material_property(white, '', unreal.MaterialProperty.MP_BASE_COLOR)
    for prop, value, y in [(unreal.MaterialProperty.MP_ROUGHNESS, 0.82, 160),
                           (unreal.MaterialProperty.MP_SPECULAR, 0.18, 300)]:
        node = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -240, y)
        node.set_editor_property('r', value)
        lib.connect_material_property(node, '', prop)
    lib.recompile_material(mat)
    if not unreal.EditorAssetLibrary.save_loaded_asset(mat):
        raise RuntimeError('Cannot save crosswalk paint material')
    return mat


def build_mesh(plan, mat):
    buffers = unreal.GeometryScriptSimpleMeshBuffers(
        vertices=plan['vertices'], triangles=plan['triangles'],
        normals=plan['normals'], uv0=plan['uvs'])
    dm = unreal.DynamicMesh()
    unreal.GeometryScript_MeshEdits.append_buffers_to_mesh(dm, buffers)
    path = MESH_DIR + '/SM_' + plan['info']['label']
    mesh = unreal.load_asset(path)
    if mesh:
        mesh.modify(True)
        _, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(
            dm, mesh, unreal.GeometryScriptCopyMeshToAssetOptions(),
            unreal.GeometryScriptMeshWriteLOD())
    else:
        mesh, outcome = unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(
            dm, path, unreal.GeometryScriptCreateNewStaticMeshAssetOptions(
                enable_collision=False, enable_nanite=False, enable_recompute_normals=False))
    if not mesh or outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
        raise RuntimeError('Cannot build ' + path + ': ' + str(outcome))
    mesh.set_material(0, mat)
    if not unreal.EditorAssetLibrary.save_loaded_asset(mesh):
        raise RuntimeError('Cannot save ' + path)
    plan['info']['mesh'] = path
    return mesh


def run(apply=True):
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if editor.get_game_world():
        raise RuntimeError('Stop PIE before rebuilding crosswalks')
    world = editor.get_editor_world()
    if not world or world.get_path_name() != '/Game/Level/TestForCharacter.TestForCharacter':
        raise RuntimeError('Open TestForCharacter; this script never switches maps')
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = [a for a in eas.get_all_level_actors() if a]
    crossings = sorted([a for a in actors if a.get_actor_label().startswith('Crosswalk_')
                        and (a.get_component_by_class(unreal.DecalComponent)
                             or SURFACE_TAG in [str(t) for t in a.tags])],
                       key=lambda a:a.get_actor_label())
    if not crossings:
        raise RuntimeError('No existing Crosswalk_* anchors; nothing will be invented')
    before = traffic_snapshot(actors)
    sampler = RoadSampler(world, actors)
    plans = [prepare(a, sampler) for a in crossings]
    report = dict(status='measured', count=len(plans), crossings=[p['info'] for p in plans])
    REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    if not apply:
        return report
    mat = paint_material()
    # Finish all assets before replacing level actors.
    meshes = [build_mesh(p, mat) for p in plans]
    created = []
    with unreal.ScopedEditorTransaction('Rebuild road-conforming crosswalk stripes'):
        try:
            for p, mesh in zip(plans, meshes):
                rot = unreal.MathLibrary.make_rot_from_xz(p['forward'], unreal.Vector(0,0,1))
                a = eas.spawn_actor_from_class(unreal.StaticMeshActor, p['origin'], rot)
                if not a:
                    raise RuntimeError('Cannot spawn ' + p['info']['label'])
                created.append(a)
                a.modify(True)
                a.set_actor_label(p['info']['label'] + '_Replacement')
                a.set_folder_path(p['actor'].get_folder_path())
                a.tags = [unreal.Name(t) for t in dict.fromkeys(
                    [str(t) for t in p['actor'].tags if str(t) != LEGACY_TAG] + [SURFACE_TAG])]
                c = a.static_mesh_component
                c.modify(True)
                c.set_static_mesh(mesh)
                c.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
                c.set_editor_property('cast_shadow', False)
                c.set_editor_property('affect_distance_field_lighting', False)
                a.set_actor_enable_collision(False)
            assert traffic_snapshot(eas.get_all_level_actors()) == before, 'Traffic changed'
        except Exception:
            for a in created:
                eas.destroy_actor(a)
            raise
        for p, a in zip(plans, created):
            p['actor'].modify(True)
            if not eas.destroy_actor(p['actor']):
                raise RuntimeError('Could not remove old marking: ' + p['info']['label'])
            a.set_actor_label(p['info']['label'])
    assert traffic_snapshot(eas.get_all_level_actors()) == before, 'Traffic changed'
    assert not any(a.get_actor_label().startswith('Crosswalk_') and
                   a.get_component_by_class(unreal.DecalComponent)
                   for a in eas.get_all_level_actors() if a), 'Old crosswalk decal remains'
    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError('Crosswalks replaced but level save failed')
    report.update(status='saved', traffic_unchanged=True,
                  old_decal_count=0, surface_material=MATERIAL)
    REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    unreal.log('Rebuilt %d crosswalks; saved mesh assets and level; traffic unchanged.' % len(plans))
    return report


if __name__ == '__main__':
    try:
        run()
    except Exception:
        unreal.log_error(traceback.format_exc())
        raise
