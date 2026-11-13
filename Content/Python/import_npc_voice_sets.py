"""导入已选的老年人反应音，再按配置填写各人物的声音资产"""
import hashlib
import json
import wave
from pathlib import Path
import unreal

root=Path(unreal.Paths.project_dir()).resolve()
folder=root/'Design/Art/NPC/Residents/Voice/ElderlyCandidates'
manifest=json.loads((folder/'Manifest.json').read_text(encoding='utf-8'))
profiles=json.loads((root/'Design/AI/NpcVoices.json').read_text(encoding='utf-8'))
clips=manifest['clips']
names=set()
reaction_config=profiles.get('short_reactions')
if reaction_config:
    # B 音色短音单独保存来源，复建时与已有真人短音一起核对
    from import_npc_short_reactions import MANIFEST, GENERATED, capture_voice_sets, read_manifest
    if (root/reaction_config['manifest']).resolve()!=MANIFEST:
        raise RuntimeError('B reaction manifest is outside the generated voice folder')
    generated=read_manifest(MANIFEST,capture_voice_sets())
    for clip in generated['clips']:
        clips.append({'asset_name':clip['asset_name'],'asset_destination':clip['asset_destination'],
                      'prepared_relative_path':clip['source_relative_path'],'prepared_sha256':clip['sha256'],
                      'sample_rate':24000,'generated_b':True})

# 完整核对清单后再导入，避免遗漏声音却继续给人物绑定
for clip in clips:
    source=(root/clip['prepared_relative_path']).resolve()
    source_folder=GENERATED if clip.get('generated_b') else folder
    if not source.is_relative_to(source_folder) or source.suffix.lower()!='.wav':
        raise RuntimeError('Voice file is outside the selected folder')
    if clip['asset_name'] in names or not clip['asset_destination'].startswith('/Game/NPC/Residents/Audio/Voices/'):
        raise RuntimeError('Invalid NPC voice asset path')
    names.add(clip['asset_name'])
    if hashlib.sha256(source.read_bytes()).hexdigest()!=clip['prepared_sha256']:
        raise RuntimeError('Voice file changed: '+source.name)
    with wave.open(str(source),'rb') as sound:
        if (sound.getnchannels(),sound.getsampwidth(),sound.getframerate())!=(1,2,clip.get('sample_rate',44100)):
            raise RuntimeError('Unexpected mono PCM16 format: '+source.name)

tools=unreal.AssetToolsHelpers.get_asset_tools()
for clip in clips:
    target=clip['asset_destination']
    existing=unreal.load_asset(target)
    if existing:
        if not isinstance(existing,unreal.SoundWave):
            raise RuntimeError('An unrelated asset already uses '+target)
        continue
    task=unreal.AssetImportTask()
    task.filename=str(root/clip['prepared_relative_path'])
    task.destination_path=target.rsplit('/',1)[0]
    task.destination_name=clip['asset_name']
    task.automated=True
    task.replace_existing=False
    task.save=True
    task.factory=unreal.SoundFactory()
    tools.import_asset_tasks([task])
    if not isinstance(unreal.load_asset(target),unreal.SoundWave):
        raise RuntimeError('Voice import failed: '+target)

fields={'heard_player','question','displeased','laugh','surprised','hit'}
selected={clip['asset_destination'] for clip in clips}
for profile in profiles['voices']:
    asset=unreal.load_asset(profile['asset'])
    if not isinstance(asset,unreal.DeliveryNpcVoiceSet):
        raise RuntimeError('Create the NPC voice assets with update_npc_profiles.py first')
    for field, sound_path in profile['sounds'].items():
        if field not in fields or sound_path not in selected:
            raise RuntimeError('Voice binding is not in the selected manifest: '+field)
        asset.set_editor_property(field,unreal.load_asset(sound_path))
    if profile.get('reaction_attenuation'):
        attenuation=unreal.load_asset(profile['reaction_attenuation'])
        if not isinstance(attenuation,unreal.SoundAttenuation):
            raise RuntimeError('NPC reaction attenuation is missing')
        asset.set_editor_property('reaction_attenuation',attenuation)
    # 保留录音原音调，音量和最长播放时间按每名人物分别设置
    asset.set_editor_property('pitch',1.0)
    asset.set_editor_property('volume',profile.get('volume',0.65))
    asset.set_editor_property('max_duration',profile.get('max_duration',1.2))
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset):
        raise RuntimeError('Could not save '+profile['asset'])
    unreal.log('NPC_VOICE_SET_UPDATED '+profile['asset'])
