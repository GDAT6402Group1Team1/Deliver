"""Verify saved pole split after reloading the level."""

import json
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(__file__))
from split_pcg_lamps_grassbeds import same_transform

REPORT = "/private/tmp/delivery_pole_split_report.json"


def main():
    report = json.load(open(REPORT, encoding="utf-8"))
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    assert levels.load_level(report["level"])
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    assert not any(a.get_actor_label() == "POLE-CREATE" for a in actors)
    poles = {a.get_actor_label(): a for a in actors if a.get_actor_label().startswith("Pole_")}
    assert len(poles) == 511, len(poles)
    for row in report["actors"]:
        actor = poles[row["label"]]
        assert str(actor.get_folder_path()) == "Poles"
        mesh = actor.get_editor_property("static_mesh_component").get_editor_property("static_mesh")
        assert mesh.get_path_name() == row["mesh"]
        expected = row["transform"]
        actual = actor.get_actor_transform()
        assert all(abs(a-b) < 0.05 for a,b in zip(expected["translation"],
            (actual.translation.x, actual.translation.y, actual.translation.z)))
        assert all(abs(a-b) < 0.0001 for a,b in zip(expected["scale"],
            (actual.scale3d.x, actual.scale3d.y, actual.scale3d.z)))
    unreal.log("[PoleSplit] reload verified: 511 independent poles in Poles, original removed")


if __name__ == "__main__":
    main()
