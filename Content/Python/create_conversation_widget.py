"""创建交谈、字幕与连麦蓝图；--speech-status-only 只处理麦克风控件"""
import sys
from pathlib import Path

import unreal


def ensure_mic_icons():
    destination = "/Game/UI/Conversation"
    art_dir = Path(unreal.Paths.project_dir()) / "Design/Art/UI/Conversation"
    for asset_name in ("T_MicOff", "T_MicOn"):
        asset_path = f"{destination}/{asset_name}"
        if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
            continue
        source = art_dir / f"{asset_name}.png"
        if not source.is_file():
            raise FileNotFoundError(source)
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", str(source.resolve()))
        task.set_editor_property("destination_path", destination)
        task.set_editor_property("destination_name", asset_name)
        task.set_editor_property("automated", True)
        task.set_editor_property("save", False)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        texture = unreal.load_asset(asset_path)
        if not isinstance(texture, unreal.Texture2D):
            raise RuntimeError(f"Mic icon could not be imported: {asset_path}")
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
        texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        if not unreal.EditorAssetLibrary.save_loaded_asset(texture):
            raise RuntimeError(f"Mic icon could not be saved: {asset_path}")


speech_status_path = "/Game/UI/Conversation/WBP_SpeechStatus"
replace_layout = "--replace-layout" in sys.argv[1:]
widgets = (
    ("/Game/UI/Conversation/WBP_Conversation",
     unreal.DeliveryConversationAssetTools.create_conversation_widget),
    ("/Game/UI/Conversation/WBP_NpcCaption",
     unreal.DeliveryNpcCaptionAssetTools.create_npc_caption_widget),
    (speech_status_path,
     unreal.DeliverySpeechAssetTools.create_speech_assets),
)
if "--speech-status-only" in sys.argv[1:]:
    widgets = widgets[-1:]
if replace_layout or not unreal.EditorAssetLibrary.does_asset_exist(speech_status_path):
    ensure_mic_icons()
for asset_path, create_widget in widgets:
    result = create_widget(replace_layout)
    unreal.log(str(result))
    if not isinstance(result, str) or (
        "saved:" not in result.lower() and "already exists" not in result.lower()
    ):
        raise RuntimeError(f"{asset_path}: {result}")
    if unreal.load_asset(asset_path) is None:
        raise RuntimeError(f"Widget asset could not be loaded: {asset_path}")
    unreal.log(f"CONVERSATION_ASSET_OK {asset_path}")

for asset_path in (
    "/Game/UI/Conversation/Input/IA_PushToTalk",
    "/Game/UI/Conversation/Input/IMC_VoiceConversation",
):
    if unreal.load_asset(asset_path) is None:
        raise RuntimeError(f"Voice input asset could not be loaded: {asset_path}")
    unreal.log(f"VOICE_INPUT_ASSET_OK {asset_path}")
