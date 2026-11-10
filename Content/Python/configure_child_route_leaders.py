# -*- coding: utf-8 -*-
"""给当前关卡中的小孩列配置中年男/女带队者，不切图、不保存"""

import unreal

MAIN_ROUTES = (
    'PedestrianRoute_ChildCrosswalk4502_A',
    'PedestrianRoute_FeaturedCrosswalkR0039_A',
    'PedestrianRoute_ChildCrosswalkR0358_A',
    'PedestrianRoute_ChildCrosswalkR0358_B',
)
TEST_ROUTES = ('PedestrianRoute_ChildQueue',)


def configure():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is None or world.get_name() not in ('TestForCharacter', 'testfortraffic'):
        raise RuntimeError('Open TestForCharacter or testfortraffic')
    names = MAIN_ROUTES if world.get_name() == 'TestForCharacter' else TEST_ROUTES
    actors = {a.get_actor_label(): a for a in
              unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()}
    routes = []
    for name in names:
        route = actors.get(name)
        if not isinstance(route, unreal.DeliveryPedestrianRoute):
            raise RuntimeError('Missing child route ' + name)
        if len(route.get_editor_property('kinds')) != 2:
            raise RuntimeError('Not a child-only route: ' + name)
        routes.append(route)
    leaders, fades = [], []
    for mesh_name, fade_name in (('SK_NpcMan', 'M_Man_Fade'),
                                 ('SK_NpcWoman', 'M_Woman_Fade')):
        mesh = unreal.load_asset('/Game/Characters/NPC/' + mesh_name)
        fade = unreal.load_asset('/Game/Characters/NPC/' + fade_name)
        if not isinstance(mesh, unreal.SkeletalMesh) or not isinstance(fade, unreal.Material):
            raise RuntimeError('Missing adult leader asset: ' + mesh_name)
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property('mesh', mesh)
        kind.set_editor_property('weight', 1.0)
        kind.set_editor_property('height_scale', 1.0)
        leaders.append(kind)
        fades.append(fade)
    for route in routes:
        route.set_editor_property('leader_kinds', leaders)
        if route.get_editor_property('fade_instead_of_scale'):
            route.set_editor_property('leader_fade_materials', fades)
    unreal.log('[ChildLeaders] Configured %d routes in %s' % (len(routes), world.get_name()))


if __name__ == '__main__':
    configure()
