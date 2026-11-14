"""读取对白和背景音乐配置；加 -ApplyDialogueMusicBalance 后保存较低的背景音乐音量"""
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import shutil

import unreal


PLAYER_CONTROLLER = '/Game/Blueprint/PlayerController/BP_DeliveryManPC'
VOICE_PATHS = (
    '/Game/NPC/Residents/Audio/DA_Voice_OldMan',
    '/Game/NPC/Residents/Audio/DA_Voice_OldWoman',
    '/Game/NPC/Companion/DA_Voice_Ergou',
)
MUSIC_VOLUME = 0.22


def path_of(asset):
    return asset.get_path_name() if asset else None


def music_settings(music):
    fields = ('music_volume', 'call_duck_volume_scale', 'duck_interp_speed',
              'crossfade_seconds', 'keep_playing_while_paused', 'music_enabled')
    result = {name: music.get_editor_property(name) for name in fields}
    for name in ('explore_track', 'task_active_track', 'overdue_track'):
        result[name] = path_of(music.get_editor_property(name))
    return result


def read_voices():
    result = {}
    for path in VOICE_PATHS:
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            result[path] = {'available': False}
            continue
        asset = unreal.load_asset(path)
        if not asset:
            result[path] = {'available': False}
            continue
        attenuation = asset.get_editor_property('dialogue_attenuation')
        entry = {'available': True, 'dialogue_volume': asset.get_editor_property('dialogue_volume'),
                 'reaction_volume': asset.get_editor_property('volume'),
                 'attenuation': path_of(attenuation)}
        if attenuation:
            settings = attenuation.get_editor_property('attenuation')
            entry.update(inner_radius_cm=settings.get_editor_property('attenuation_shape_extents').x,
                         falloff_cm=settings.get_editor_property('falloff_distance'))
        result[path] = entry
    return result


def run():
    apply = '-ApplyDialogueMusicBalance' in unreal.SystemLibrary.get_command_line()
    root = Path(unreal.Paths.project_dir()).resolve()
    output = root / 'Saved/AIResearch/DialogueMusicBalance'
    output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    blueprint = unreal.load_asset(PLAYER_CONTROLLER)
    if not isinstance(blueprint, unreal.Blueprint):
        raise RuntimeError('找不到玩家控制器蓝图')
    cdo = unreal.get_default_object(blueprint.generated_class())
    music = cdo.get_component_by_class(unreal.DeliveryMusicComponent)
    if not music:
        raise RuntimeError('玩家控制器缺少已有音乐组件')
    before = music_settings(music)
    voices = read_voices()
    if not math.isfinite(before['music_volume']) or not 0 <= before['music_volume'] <= 2:
        raise RuntimeError('现有音乐音量无效，未修改资产')
    report = {'status': 'read_only', 'blueprint': PLAYER_CONTROLLER,
              'before': before, 'voices': voices, 'target_music_volume': MUSIC_VOLUME}
    if apply and before['music_volume'] > MUSIC_VOLUME + 0.0001:
        # 保存原资产，调整仍保留在 Music 组件的可编辑参数中
        source = root / 'Content/Blueprint/PlayerController/BP_DeliveryManPC.uasset'
        backup = output / (stamp + '-BP_DeliveryManPC.uasset')
        shutil.copy2(source, backup)
        report['backup'] = str(backup)
        report['before_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
        music.set_editor_property('music_volume', MUSIC_VOLUME)
        after = music_settings(music)
        if {k: v for k, v in before.items() if k != 'music_volume'} != {
                k: v for k, v in after.items() if k != 'music_volume'}:
            raise RuntimeError('音乐的其它配置发生变化，未保存')
        if read_voices() != voices:
            raise RuntimeError('对白配置发生变化，未保存')
        if not unreal.EditorAssetLibrary.save_loaded_asset(blueprint, only_if_is_dirty=False):
            raise RuntimeError('玩家控制器音量保存失败')
        report['status'] = 'saved'
        report['after_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
    elif apply:
        report['status'] = 'already_configured'
    report['after'] = music_settings(music)
    (output / (stamp + '.json')).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    (output / 'latest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    unreal.log('DELIVERY_DIALOGUE_MUSIC ' + json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    run()
