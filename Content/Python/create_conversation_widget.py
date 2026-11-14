"""创建交谈、字幕与连麦蓝图；仅 --replace-layout 显式重建已有布局"""
import sys

import unreal

replace_layout = "--replace-layout" in sys.argv[1:]
for asset_path, create_widget in (
    ("/Game/UI/Conversation/WBP_Conversation",
     unreal.DeliveryConversationAssetTools.create_conversation_widget),
    ("/Game/UI/Conversation/WBP_NpcCaption",
     unreal.DeliveryNpcCaptionAssetTools.create_npc_caption_widget),
    ("/Game/UI/Conversation/WBP_SpeechStatus",
     unreal.DeliverySpeechAssetTools.create_speech_assets),
):
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
