"""检查两名街坊的导航；传入 --repair 才补全空导航体积并保存测试地图"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


MAP = '/Game/Level/TestForCharacter'
ROOT = Path(__file__).resolve().parents[2]


def ensure_editor_closed():
    command = "Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'UnrealEditor.exe' -and $_.CommandLine -match 'Delivery\\.uproject' } | Select-Object -ExpandProperty ProcessId"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', command],
                            capture_output=True, text=True, check=True)
    if result.stdout.strip():
        raise RuntimeError('Delivery 编辑器正在运行，停止导航修复')


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def main(repair=False):
    import unreal
    if repair:
        ensure_editor_closed()
    folder = ROOT / 'Saved/AIResearch/NpcNavigationRepair' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    folder.mkdir(parents=True, exist_ok=False)
    world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
    if not world:
        raise RuntimeError('无法加载测试地图')
    tool = unreal.DeliveryNpcToolset
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    def transform_values(actor):
        # Python 的结构体文本包含临时内存地址，比较实际数值才能判断物件是否移动
        value = actor.get_actor_transform()
        return {'translation': [value.translation.x, value.translation.y, value.translation.z],
                'rotation': [value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w],
                'scale': [value.scale3d.x, value.scale3d.y, value.scale3d.z]}

    def other_transforms():
        # 允许目标导航体积和导航数据更新，其他人物与场景物件保持原位
        return {actor.get_path_name(): transform_values(actor)
                for actor in actors.get_all_level_actors()
                if actor.get_actor_label() != 'Resident_Navigation'
                and not isinstance(actor, unreal.NavigationData)}

    before = json.loads(tool.check_npc_navigation(False))
    write_json(folder / 'before.json', before)
    if not repair:
        unreal.log('NPC_NAVIGATION_REPORT=' + str(folder))
        return
    if before.get('world') != MAP or before.get('status') != 'inspected':
        raise RuntimeError('地图或目标数量不符合要求，保留检查记录并停止')
    source = ROOT / 'Content/Level/TestForCharacter.umap'
    shutil.copy2(source, folder / 'TestForCharacter.before.umap')
    original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    original_transforms = other_transforms()
    write_json(folder / 'actor-transforms-before.json', original_transforms)
    after = json.loads(tool.check_npc_navigation(True))
    write_json(folder / 'after.json', after)
    current_transforms = other_transforms()
    write_json(folder / 'actor-transforms-after.json', current_transforms)
    if after.get('status') not in ('repaired_empty_bounds', 'rebuilt_existing_bounds'):
        raise RuntimeError('导航未修复，地图未保存：' + after.get('status', 'unknown'))
    state = after.get('after', {})
    if not state.get('valid_brush') or len(state.get('npcs', [])) != 2:
        raise RuntimeError('导航体积检查失败，地图未保存')
    if not all(npc.get('inside_bounds') and npc.get('on_navigation')
               and npc.get('complete_path_to_other') for npc in state['npcs']):
        raise RuntimeError('NPC 之间仍没有完整路径，地图未保存；检查 after.json')
    if current_transforms != original_transforms:
        raise RuntimeError('其他物件的位置有变化，地图未保存')
    ensure_editor_closed()
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
        raise RuntimeError('导航检查通过，但地图保存失败')
    report = {'folder': str(folder), 'saved_map': MAP, 'status': after['status'],
              'before_sha256': original_hash,
              'after_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'other_actor_transforms_unchanged': True}
    write_json(folder / 'result.json', report)
    unreal.log('NPC_NAVIGATION_REPORT=' + str(folder))


if __name__ == '__main__':
    main('--repair' in sys.argv)
