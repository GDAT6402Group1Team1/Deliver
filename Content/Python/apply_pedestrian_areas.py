"""Replace the old connected test routes with four local pedestrian areas."""

import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(__file__))
import setup_npc

level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not level.load_level("/Game/Level/testfortraffic"):
    raise RuntimeError("Cannot load testfortraffic")

areas = setup_npc.place_route()
if areas is None or len(areas) != len(setup_npc.PEDESTRIAN_AREAS):
    raise RuntimeError("Not all pedestrian areas were placed")
if not level.save_current_level():
    raise RuntimeError("Cannot save testfortraffic")

unreal.log("[npc] Saved %d separate roaming areas" % len(areas))
