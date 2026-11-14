"""保存教师与人工复核后的训练数据，内部验证继续使用提前固定的答案"""
import argparse
from collections import Counter
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from combat_model import COMBAT_INSTRUCTIONS, prepare_combat_context
from build_combat_data import ROOT, write_json


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_lines(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines() if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, required=True)
    parser.add_argument('--review', type=Path, required=True)
    args = parser.parse_args()
    folder = args.folder
    rows = json.loads((folder / 'situations.json').read_text(encoding='utf-8'))
    report = json.loads((folder / 'teacher.json').read_text(encoding='utf-8'))
    review = json.loads(args.review.read_text(encoding='utf-8'))
    assert report['situations_sha256'] == sha(folder / 'situations.json')
    assert review['teacher_sha256'] == sha(folder / 'teacher.json')
    assert review['validation_labels_changed'] is False
    by_id = {r['id']: r for r in report['results']}
    checked = {r['id']: r for r in review['training_disagreements']}
    previous = ROOT / 'Saved/AIResearch/NpcTraining/combat-v5'
    train = read_lines(previous / 'train.jsonl')
    validation = read_lines(previous / 'validation.jsonl')
    assert len(train) == 160 and len(validation) == 15
    new_validation = []
    for row in rows:
        if not row['id'].startswith('change-'):
            continue
        teacher = by_id[row['id']]
        assert teacher['input_ok'] is True
        prompt, _ = prepare_combat_context(row['context'])
        assert prompt == row['prompt']
        draft = row['label']['acceptable_actions']
        assert len(draft) == 1
        if row['usage'] == 'train':
            action = teacher['action']
            if action not in draft:
                decision = checked[row['id']]
                assert decision['use_teacher_label'] is True and decision['reason']
            method = 'teacher_review_with_independent_disagreement_check'
        else:
            action = draft[0]
            method = 'author_label_fixed_before_student_baseline'
        record = {'id': row['id'], 'group_id': row['group_id'], 'source': row['source'],
                  'task': 'combat_update', 'target_label_source': method,
                  'teacher_review': deepcopy(teacher),
                  'messages': [{'role': 'system', 'content': COMBAT_INSTRUCTIONS},
                               {'role': 'user', 'content': json.dumps(prompt, ensure_ascii=False, separators=(',', ':'))},
                               {'role': 'assistant', 'content': json.dumps({'action': action, 'target_id': '' if action == 'Wait' else 'opponent'}, separators=(',', ':'))}]}
        if row['usage'] == 'train':
            train.append(record)
        else:
            validation.append(record)
            new_validation.append(record)
    assert len(train) == 224 and len(validation) == 31 and len(new_validation) == 16
    assert not {r['group_id'] for r in train} & {r['group_id'] for r in validation}
    for name, records in (('train', train), ('validation', validation), ('validation-new', new_validation)):
        (folder / (name + '.jsonl')).write_text(''.join(json.dumps(r, ensure_ascii=False) + '\n' for r in records), encoding='utf-8')
    summary = {'status': 'data_ready_for_training', 'teacher_sha256': sha(folder / 'teacher.json'),
               'review_sha256': sha(args.review), 'situations_sha256': sha(folder / 'situations.json'),
               'train_sha256': sha(folder / 'train.jsonl'), 'validation_sha256': sha(folder / 'validation.jsonl'),
               'train_rows': len(train), 'train_groups': len({r['group_id'] for r in train}),
               'legacy_validation_rows': 15, 'new_validation_rows': len(new_validation),
               'validation_labels_changed': False, 'new_train_teacher_disagreements': len(checked),
               'train_actions': dict(Counter(json.loads(r['messages'][-1]['content'])['action'] for r in train)),
               'input_preprocessing_version': 'self_combat_actual_facts_v2'}
    write_json(folder / 'data-ready.json', summary)
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == '__main__':
    main()
