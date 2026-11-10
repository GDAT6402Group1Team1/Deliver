# -*- coding: utf-8 -*-
"""清理已删除的 R037_4 斑马线留下的停车 tag 和两处路人活动区"""
import unreal

LEVEL = '/Game/Level/TestForCharacter'
DELETED_MARK = 'Crosswalk_BranchRoad_R037_4'
SEGMENT = 'Inter_BranchRoad_R037_-0180_I03'
AREAS = ('PedestrianRoute_Crosswalk_A', 'PedestrianRoute_Crosswalk_B')

level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not level.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = {a.get_actor_label(): a for a in editor.get_all_level_actors()}
if DELETED_MARK in actors:
    raise RuntimeError('Crosswalk still exists: ' + DELETED_MARK)

segment = actors.get(SEGMENT)
if segment is None:
    raise RuntimeError('Missing traffic segment: ' + SEGMENT)
tags = [str(tag) for tag in segment.tags]
if 'Crosswalk' in tags:
    segment.tags = [unreal.Name(tag) for tag in tags if tag != 'Crosswalk']
    unreal.log('[RemoveR037Crosswalk] removed Crosswalk tag from ' + SEGMENT)

removed = 0
for label in AREAS:
    actor = actors.get(label)
    if actor is None:
        continue
    if 'DeliveryCentralPedestrianAreaV1' not in [str(tag) for tag in actor.tags]:
        raise RuntimeError('Unexpected actor with same label: ' + label)
    if not editor.destroy_actor(actor):
        raise RuntimeError('Could not remove ' + label)
    removed += 1

if not level.save_current_level():
    raise RuntimeError('Could not save level')
unreal.log('[RemoveR037Crosswalk] saved; removed %d orphan pedestrian areas' % removed)
