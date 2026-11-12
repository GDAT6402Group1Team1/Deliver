# -*- coding: utf-8 -*-
"""将清单中的真人短反应音导入独立目录，已有音频保持原配置"""
import hashlib
import json
import wave
from pathlib import Path

import unreal


ROOT = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
PREPARED = ROOT / "Design/Art/NPC/Residents/Voice/Prepared"
DESTINATION = "/Game/NPC/Residents/Audio/Reactions"


def expected_assets():
    names = set()
    for category in ("Effort", "Hurt", "Jump", "Relief"):
        for number in (1, 2, 3):
            names.add(f"SW_NpcFemale_{category}_{number:02d}")
    for category, numbers in (("Jump", (0, 1)), ("Effort", (0, 1, 2)), ("Hurt", (0, 1, 8))):
        for number in numbers:
            names.add(f"SW_NpcMale_{category}_{number:02d}")
    return names


def read_clips():
    manifest = json.loads((PREPARED / "Manifest.json").read_text(encoding="utf-8"))
    clips = manifest["clips"]
    names = [clip["asset_name"] for clip in clips]
    if manifest.get("schema_version") != 1 or len(names) != 20 or set(names) != expected_assets():
        raise RuntimeError("音频清单与选定的 20 条短反应音不一致")

    # 创建资产前检查全部文件，避免缺少音频时只导入其中一部分
    for clip in clips:
        voice = clip["voice"]
        name = clip["asset_name"]
        source = (ROOT / clip["prepared_relative_path"]).resolve()
        if voice not in ("Female", "Male") or not name.startswith(f"SW_Npc{voice}_"):
            raise RuntimeError(f"人物分类与文件名不一致：{name}")
        if source.parent != PREPARED.resolve() or source.name != name + ".wav":
            raise RuntimeError(f"音频文件不在 Prepared 目录：{source}")
        target = f"{DESTINATION}/{voice}/{name}"
        if clip["asset_destination"] != target or clip["license"] != "CC0-1.0":
            raise RuntimeError(f"导入路径或许可记录不一致：{name}")
        if hashlib.sha256(source.read_bytes()).hexdigest() != clip["prepared_sha256"]:
            raise RuntimeError(f"音频文件已变化，请更新清单后再导入：{name}")
        with wave.open(str(source), "rb") as audio:
            expected = (1, 2, 44100, clip["frames"])
            actual = (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getnframes())
            if actual != expected:
                raise RuntimeError(f"音频格式或长度不一致：{name}")
    return clips


def run():
    clips = read_clips()
    tasks = []
    for clip in clips:
        target = clip["asset_destination"]
        if unreal.EditorAssetLibrary.does_asset_exist(target):
            existing = unreal.EditorAssetLibrary.load_asset(target)
            if not isinstance(existing, unreal.SoundWave):
                raise RuntimeError(f"目标路径已有其他类型的资产：{target}")
            unreal.log(f"[NpcReactionAudio] 已有音频，保持原配置：{target}")
            continue

        task = unreal.AssetImportTask()
        task.set_editor_property("filename", str(ROOT / clip["prepared_relative_path"]))
        task.set_editor_property("destination_path", f"{DESTINATION}/{clip['voice']}")
        task.set_editor_property("destination_name", clip["asset_name"])
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", False)
        task.set_editor_property("save", True)
        task.set_editor_property("factory", unreal.SoundFactory())
        tasks.append(task)

    if tasks:
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    # 只检查音频资产，不给角色、蓝图或地图绑定声音
    for clip in clips:
        sound = unreal.EditorAssetLibrary.load_asset(clip["asset_destination"])
        if not isinstance(sound, unreal.SoundWave):
            raise RuntimeError(f"音频导入失败：{clip['asset_destination']}")
    unreal.log(f"[NpcReactionAudio] 清单内 20 条音频检查完成，本次新导入 {len(tasks)} 条")


if __name__ == "__main__":
    run()
