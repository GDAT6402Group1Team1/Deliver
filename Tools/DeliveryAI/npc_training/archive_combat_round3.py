"""保存第三轮的可复核记录，模型权重、缓存和整段游戏日志不进入源码仓库"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'Saved/AIResearch/NpcTraining'
RECORDS = ROOT / 'Tools/DeliveryAI/npc_training/records/2026-11-14'
TARGET = RECORDS / 'round3'
FROZEN_SHA = '77706e89104e4ec6d12704cb5b2214ff8efa9e4117cded6e8c400adf3acacced'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_record(source):
    text = source.read_text(encoding='utf-8')
    if re.search(r'\b(?:sk|pk)-[A-Za-z0-9_-]{24,}|Bearer\s+[A-Za-z0-9._~-]{20,}', text):
        raise ValueError('疑似凭据，停止归档: ' + str(source))
    if source.suffix not in ('.json', '.jsonl'):
        return
    values = [json.loads(line) for line in text.splitlines() if line.strip()] if source.suffix == '.jsonl' else [json.loads(text)]
    def visit(value):
        if isinstance(value, dict):
            for key, entry in value.items():
                if key.lower() in ('api_key', 'access_token', 'refresh_token', 'password', 'secret') and entry:
                    raise ValueError('发现敏感配置，停止归档: ' + str(source))
                # authorization 在实验计划中是任务授权说明，只有认证头才视为凭据
                if key.lower() == 'authorization' and isinstance(entry, str) and re.match(r'(?i)(Bearer|Basic)\s', entry):
                    raise ValueError('发现认证头，停止归档: ' + str(source))
                visit(entry)
        elif isinstance(value, list):
            for entry in value:
                visit(entry)
    for value in values:
        visit(value)


def main():
    if TARGET.exists():
        raise FileExistsError('保留已有归档，不覆盖: ' + str(TARGET))
    old_files = {str(path.relative_to(RECORDS)): sha(path) for path in RECORDS.rglob('*') if path.is_file()}
    selected = []
    for name in ('combat-v6', 'combat-v7', 'round3-baseline'):
        folder = SOURCE / name
        for path in sorted(folder.rglob('*')):
            if not path.is_file() or any(part in ('merged', 'adapter', 'tokenizer') for part in path.relative_to(folder).parts):
                continue
            if path.suffix in ('.json', '.jsonl', '.py') or path.name == 'MODEL_LICENSE':
                selected.append((path, TARGET / path.relative_to(SOURCE)))
    for name in ('convert', 'quantize'):
        selected.append((SOURCE / f'combat-v7/export-round3/{name}.log', TARGET / f'combat-v7/export-round3/{name}.txt'))
    for relative in ('Tools/DeliveryAI/combat_model.py',
                     'Tools/DeliveryAI/npc_training/build_combat_round3.py',
                     'Tools/DeliveryAI/npc_training/combat_round3_stories.py',
                     'Tools/DeliveryAI/npc_training/finalize_combat_round3.py',
                     'Tools/DeliveryAI/npc_training/archive_combat_round3.py',
                     'Tools/DeliveryAI/npc_training/review_combat_data.py',
                     'Tools/DeliveryAI/npc_training/train_lora.py',
                     'Tools/DeliveryAI/npc_training/evaluate_combat.py',
                     'Tools/DeliveryAI/npc_training/export_combat_model.py',
                     'Tools/DeliveryAI/llm_evaluation/start_local_model.py'):
        selected.append((ROOT / relative, TARGET / 'source-snapshots' / Path(relative).name))
    for source, _ in selected:
        inspect_record(source)
    assert sha(SOURCE / 'acceptance-reviewed.json') == FROZEN_SHA
    TARGET.mkdir(parents=True)
    entries = []
    for source, destination in selected:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        assert sha(source) == sha(destination)
        entries.append({'source': source.relative_to(ROOT).as_posix(),
                        'archive': destination.relative_to(TARGET).as_posix(),
                        'bytes': destination.stat().st_size, 'sha256': sha(destination)})
    manifest = {
        'created_utc': datetime.now(timezone.utc).isoformat(),
        'kind': 'additional_round3_records_after_explicit_user_instruction',
        'old_record_count': len(old_files), 'older_records_modified': False,
        'frozen_acceptance_copy': '../acceptance-reviewed.json', 'frozen_acceptance_sha256': FROZEN_SHA,
        'files': entries,
        'input_source_hashes': {
            'before_fix_at_this_baseline': None,
            'before_fix_note': '本轮修复前未实时记录完整源码哈希；保留 v6 的实际模型输入和转换前后哈希，不反推历史源码',
            'previous_round_recorded_hash': '934972d07d51c3c8fdf2e62a65787e2920fb2c53c2235a786fef6c07f16fefb5',
            'previous_round_hash_is_not_new_measurement': True,
            'after_fix': sha(ROOT / 'Tools/DeliveryAI/combat_model.py')},
        'privacy_review': {
            'content': '虚构 NPC 情境、此前已检查的少量游戏回放样本、教师分类和训练评测数据，以及本轮脚本快照',
            'full_user_chat_logs_included': False, 'credentials_included': False,
            'credential_patterns_and_nonempty_sensitive_keys_found': 0, 'weights_or_caches_included': False},
        'limitations': [
            '40/40 是同一份封存验收题的第三次评测，不是新盲测',
            '新增16个内部情境仍为12/16，草案标签未改',
            '这轮同时修正模型输入并继续训练，不能把全部变化算作训练收益',
            '独立决策耗时没有同时运行游戏和语音',
            '漏传teacher参数的错误评测记录保留，补齐参数后输出动作逐项相同'],
        'commands': {
            'training': '见 combat-v7/experiment-plan.json 的 fixed_training.command',
            'teacher': 'python Tools/DeliveryAI/npc_training/review_combat_data.py --folder Saved/AIResearch/NpcTraining/combat-v7 --batch-size 6 --run',
            'finalization': 'python Tools/DeliveryAI/npc_training/finalize_combat_round3.py --folder Saved/AIResearch/NpcTraining/combat-v7 --review Saved/AIResearch/NpcTraining/combat-v7/label-review.json',
            'export': 'Saved/AIResearch/NpcTraining/.venv/Scripts/python.exe Tools/DeliveryAI/npc_training/export_combat_model.py --adapter Saved/AIResearch/NpcTraining/combat-v7/round3/adapter --output Saved/AIResearch/NpcTraining/combat-v7/export-round3 --quantization Q8_0 --run',
            'acceptance': 'python Tools/DeliveryAI/npc_training/evaluate_combat.py --source Saved/AIResearch/NpcTraining/acceptance-reviewed.json --output Saved/AIResearch/NpcTraining/combat-v7/final-acceptance-q8-gpu99.json --split acceptance_only --temperature 0 --seed 20261114 --timeout 1.5 --run'}}
    manifest_file = TARGET / 'round3-manifest.json'
    manifest_file.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    assert all(sha(RECORDS / name) == value for name, value in old_files.items())
    print(json.dumps({'archive': str(TARGET), 'files_including_manifest': len(entries) + 1,
                      'bytes': sum(e['bytes'] for e in entries) + manifest_file.stat().st_size,
                      'manifest_sha256': sha(manifest_file), 'old_files_unchanged': len(old_files)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
