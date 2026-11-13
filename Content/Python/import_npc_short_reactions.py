"""读取或补齐 B 音色短音；默认只读，main(apply=True, manifest=...) 才导入并绑定"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import wave

import unreal


ROOT = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
RESULTS = ROOT / 'Saved/AIResearch/NpcShortReactions_20261113'
GENERATED = ROOT / 'Design/Art/NPC/Residents/Voice/Generated/ShortReactions_B'
MANIFEST = GENERATED / 'Manifest.json'
VOICE_CONFIG = ROOT / 'Design/AI/NpcVoices.json'
ASSETS = {
    'male': '/Game/NPC/Residents/Audio/DA_Voice_OldMan',
    'female': '/Game/NPC/Residents/Audio/DA_Voice_OldWoman',
}
ATTENUATION = '/Game/NPC/Residents/Audio/SA_NpcDialogue'
SOUND_FIELDS = ('heard_player', 'question', 'displeased', 'laugh', 'surprised', 'hit')
OBJECT_FIELDS = (*SOUND_FIELDS, 'dialogue_attenuation', 'reaction_attenuation')
VALUE_FIELDS = ('dialogue_volume', 'volume', 'pitch', 'max_duration')
EXPECTED = {
    ('male', 'heard_player'): ('嗯？', 'SW_NpcMale_B_HeardPlayer', 'Uncle_Fu'),
    ('male', 'hit'): ('哎哟！', 'SW_NpcMale_B_Hit', 'Uncle_Fu'),
    ('female', 'heard_player'): ('嗯？', 'SW_NpcFemale_B_HeardPlayer', 'Xiaofang_B'),
    ('female', 'hit'): ('哎哟！', 'SW_NpcFemale_B_Hit', 'Xiaofang_B'),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def package_file(asset_path):
    if not asset_path.startswith('/Game/'):
        raise RuntimeError('只处理本项目声音资源')
    return ROOT / 'Content' / (asset_path[len('/Game/'):] + '.uasset')


def object_path(value):
    return value.get_path_name() if value else None


def capture_voice_sets():
    result = {}
    for voice, path in ASSETS.items():
        asset = unreal.load_asset(path)
        if not isinstance(asset, unreal.DeliveryNpcVoiceSet):
            raise RuntimeError('未找到正式声音配置：' + path)
        result[voice] = {'asset': path, **{key: object_path(asset.get_editor_property(key)) for key in OBJECT_FIELDS},
                         **{key: asset.get_editor_property(key) for key in VALUE_FIELDS}}
    return result


def capture_short_sounds():
    result = {}
    for voice, path in ASSETS.items():
        asset = unreal.load_asset(path)
        for field in ('heard_player', 'hit'):
            sound = asset.get_editor_property(field)
            if not sound:
                continue
            if not isinstance(sound, unreal.SoundWave):
                raise RuntimeError('短音应为独立 SoundWave：' + object_path(sound))
            source = Path(sound.get_editor_property('asset_import_data').get_first_filename()).resolve()
            result[voice + '.' + field] = {'asset': object_path(sound), 'duration_s': sound.get_editor_property('duration'),
                                          'source': str(source), 'source_sha256': digest(source) if source.is_file() else None}
    return result


def read_manifest(path, before):
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('schema_version') != 1 or data.get('status') != 'generated_pending_review' or not data.get('worker_closed'):
        raise RuntimeError('候选生成没有完整结束')
    clips = data['clips']
    if len(clips) != 4 or {(item['voice'], item['reaction']) for item in clips} != set(EXPECTED):
        raise RuntimeError('清单必须包含男女各一条听见玩家和受击短音')
    for clip in clips:
        text, name, speaker = EXPECTED[(clip['voice'], clip['reaction'])]
        source = (ROOT / clip['source_relative_path']).resolve()
        if not (source.is_relative_to(RESULTS) or source.is_relative_to(GENERATED)) or source.suffix != '.wav':
            raise RuntimeError('声音来源不在本次候选目录：' + str(source))
        target = '/Game/NPC/Residents/Audio/Voices/ShortReactions/' + name
        if (clip['text'], clip['asset_name'], clip['voice_name'], clip['asset_destination']) != (text, name, speaker, target):
            raise RuntimeError('声音身份、内容或资源路径与四段短音不一致')
        if digest(source) != clip['sha256'] or clip.get('clipped_samples', 1) != 0:
            raise RuntimeError('WAV 已变化或存在削波：' + name)
        if clip.get('original'):
            original = (ROOT / clip['original']['source_relative_path']).resolve()
            if not (original.is_relative_to(RESULTS) or original.is_relative_to(GENERATED)) or digest(original) != clip['original']['sha256']:
                raise RuntimeError('原始声音文件与处理记录不一致：' + name)
        with wave.open(str(source), 'rb') as audio:
            actual = (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getnframes())
            if actual != (1, 2, 24000, clip['frames']):
                raise RuntimeError('短音必须保持原始单声道 PCM16 / 24000 Hz：' + name)
            duration = audio.getnframes() / audio.getframerate()
            if not .1 <= duration <= before[clip['voice']]['max_duration']:
                raise RuntimeError('声音超出当前播放时长，不能裁掉尾字后直接接入：' + name)
    return data


def backup(paths, folder):
    result = []
    for source in dict.fromkeys(paths):
        relative = source.resolve().relative_to(ROOT)
        row = {'path': str(relative).replace('\\', '/'), 'existed': source.exists()}
        if source.exists():
            target = folder / 'backup' / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            row.update(backup=str(target), sha256=digest(source))
        result.append(row)
    write_json(folder / 'backup.json', result)
    return result


def verify_preserved(before, after, clips):
    expected = json.loads(json.dumps(before))
    for voice in ASSETS:
        expected[voice]['reaction_attenuation'] = ATTENUATION + '.SA_NpcDialogue'
    for clip in clips:
        expected[clip['voice']][clip['reaction']] = clip['asset_destination'] + '.' + clip['asset_name']
    if after != expected:
        raise RuntimeError('声音配置与预期不一致，保留前后报告供恢复；没有忽略其他字段差异')


def main(apply=False, manifest=None, verify_against=None):
    folder = RESULTS / 'UEImport' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    folder.mkdir(parents=True, exist_ok=False)
    before = capture_voice_sets()
    write_json(folder / 'before.json', before)
    if not apply:
        if verify_against:
            expected = json.loads(Path(verify_against).read_text(encoding='utf-8'))
            if before != expected:
                raise RuntimeError('独立读回与导入后记录不一致')
        write_json(folder / 'result.json', {'status': 'read_only_verified', 'voice_sets': before,
                                          'short_sounds': capture_short_sounds()})
        unreal.log('NPC_SHORT_REACTIONS_READBACK ' + str(folder))
        return folder

    path = Path(manifest) if manifest else MANIFEST
    if not path.is_absolute():
        path = ROOT / path
    data = read_manifest(path, before)
    profiles = json.loads(VOICE_CONFIG.read_text(encoding='utf-8'))
    for asset_path in ASSETS.values():
        if sum(item['asset'] == asset_path for item in profiles['voices']) != 1:
            raise RuntimeError('正式声音配置表中应各有一个男女声音条目')
    attenuation = unreal.load_asset(ATTENUATION)
    if not isinstance(attenuation, unreal.SoundAttenuation):
        raise RuntimeError('未找到现有对白距离衰减配置')
    clips = data['clips']
    assets = {voice: unreal.load_asset(asset) for voice, asset in ASSETS.items()}
    dirty_packages = set(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages())
    for voice, asset in assets.items():
        if asset.get_outermost() in dirty_packages:
            raise RuntimeError('声音配置有未保存编辑，先保存或关闭后再导入：' + ASSETS[voice])

    # 所有输入检查结束后备份，只修改四个新声音和两个正式声音配置
    files = [package_file(asset) for asset in ASSETS.values()] + [MANIFEST, VOICE_CONFIG]
    for clip in clips:
        files.extend((package_file(clip['asset_destination']), GENERATED / (clip['asset_name'] + '.wav')))
        if clip.get('original'):
            files.append(GENERATED / 'Original' / (clip['asset_name'] + '.wav'))
    backup(files, folder)
    try:
        GENERATED.mkdir(parents=True, exist_ok=True)
        for clip in clips:
            if clip.get('original'):
                original = ROOT / clip['original']['source_relative_path']
                original_target = GENERATED / 'Original' / (clip['asset_name'] + '.wav')
                original_target.parent.mkdir(parents=True, exist_ok=True)
                if original_target.exists() and digest(original_target) != clip['original']['sha256']:
                    raise RuntimeError('已保存不同的原始声音，不自动覆盖：' + original_target.name)
                if original.resolve() != original_target.resolve():
                    shutil.copy2(original, original_target)
                clip['original']['source_relative_path'] = str(original_target.relative_to(ROOT)).replace('\\', '/')
            source = (ROOT / clip['source_relative_path']).resolve()
            target = GENERATED / (clip['asset_name'] + '.wav')
            if target.exists() and digest(target) != clip['sha256']:
                raise RuntimeError('正式生成目录已有不同声音，不自动覆盖：' + target.name)
            if source != target:
                shutil.copy2(source, target)
            clip['source_relative_path'] = str(target.relative_to(ROOT)).replace('\\', '/')
            existing = unreal.load_asset(clip['asset_destination'])
            if existing:
                if not isinstance(existing, unreal.SoundWave):
                    raise RuntimeError('目标路径已有其他资源：' + clip['asset_destination'])
                source_data = existing.get_editor_property('asset_import_data')
                imported = Path(source_data.get_first_filename()).resolve()
                if imported != target.resolve() or not imported.is_file() or digest(imported) != clip['sha256']:
                    raise RuntimeError('已有声音来源不同，不自动覆盖：' + clip['asset_name'])
            else:
                task = unreal.AssetImportTask()
                task.filename = str(target)
                task.destination_path = clip['asset_destination'].rsplit('/', 1)[0]
                task.destination_name = clip['asset_name']
                task.automated = True
                task.replace_existing = False
                task.save = True
                task.factory = unreal.SoundFactory()
                unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
            sound = unreal.load_asset(clip['asset_destination'])
            if not isinstance(sound, unreal.SoundWave) or abs(sound.get_editor_property('duration') - clip['duration_s']) > .01:
                raise RuntimeError('短音导入类型或时长不一致：' + clip['asset_name'])

        for clip in clips:
            assets[clip['voice']].set_editor_property(clip['reaction'], unreal.load_asset(clip['asset_destination']))
        for asset in assets.values():
            asset.set_editor_property('reaction_attenuation', attenuation)
        after = capture_voice_sets()
        write_json(folder / 'after.json', after)
        verify_preserved(before, after, clips)
        for voice, asset in assets.items():
            if not unreal.EditorAssetLibrary.save_loaded_asset(asset):
                raise RuntimeError('声音配置保存失败：' + ASSETS[voice])
        data['listening_review'] = '沿用已认可的 B 音色，短音听感待用户试玩'
        data['bindings'] = ASSETS
        data['reaction_attenuation'] = ATTENUATION
        write_json(MANIFEST, data)
        for profile in profiles['voices']:
            voice = next((voice for voice, asset_path in ASSETS.items() if asset_path == profile['asset']), None)
            if not voice:
                continue
            for clip in clips:
                if clip['voice'] == voice:
                    profile['sounds'][clip['reaction']] = clip['asset_destination']
            profile['reaction_attenuation'] = ATTENUATION
        profiles['short_reactions'] = {
            'manifest': str(MANIFEST.relative_to(ROOT)).replace('\\', '/'),
            'rebuild_script': 'Content/Python/import_npc_voice_sets.py',
            'source': '使用完整对白已认可的两种 B 音色生成；保留原始 WAV，导入版仅调整振幅和缩短确定的开头静音，处理参数见清单',
            'review': '短音听感和多人游戏效果待玩家试玩'}
        write_json(VOICE_CONFIG, profiles)
        write_json(folder / 'result.json', {'status': 'imported', 'voice_sets': after,
                   'manifest': str(MANIFEST), 'short_sounds': capture_short_sounds(), 'game_verified': False,
                   'preserved_fields': ['dialogue_volume', 'dialogue_attenuation', 'volume', 'pitch', 'max_duration',
                                        'question', 'displeased', 'laugh', 'surprised']})
    except BaseException as error:
        # 保存失败时不直接覆盖 UE 已加载的包，退出进程后可按 backup.json 恢复
        write_json(folder / 'failure.json', {'error': str(error), 'restore_instructions':
                   '退出 UE 后按 backup.json 恢复已有文件；本轮新增文件在 existed=false 条目中列出'})
        raise
    unreal.log('NPC_SHORT_REACTIONS_IMPORTED ' + str(folder))
    return folder


if __name__ == '__main__':
    main()
