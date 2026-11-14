"""教师重看学生实际选错的训练输入，原输出和纠正理由分别保存"""
import argparse
import json
from pathlib import Path
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app_server import AppServer
from build_combat_data import DEFAULT, write_json
from review_combat_data import TEACHER, parse_output, prepare_training


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, default=DEFAULT)
    parser.add_argument('--student-results', type=Path, required=True)
    parser.add_argument('--run', action='store_true')
    args = parser.parse_args()
    rows = json.loads((args.folder / 'situations.json').read_text(encoding='utf-8'))
    by_id = {r['id']: r for r in rows}
    teacher = json.loads((args.folder / 'teacher.json').read_text(encoding='utf-8'))
    results = json.loads(args.student_results.read_text(encoding='utf-8'))
    if results['split'] != 'train':
        raise ValueError('教师纠错只读取训练样例，验证和验收不能回灌')
    wrong = [r for r in results['results'] if not r['correct'] and r['repeat'] == 0]
    if any(by_id[r['id']]['usage'] != 'train' for r in wrong):
        raise ValueError('学生输出含有非训练组')
    print(json.dumps({'student_errors': len(wrong), 'source': str(args.student_results), 'gpu_used': False}), flush=True)
    if not args.run:
        return
    destination = args.folder / 'student-corrections.json'
    if destination.exists():
        raise FileExistsError(destination)
    service = AppServer()
    audit = {'kind': 'offline_teacher_correction_of_student_outputs', 'on_policy_gameplay': False,
             'student_results': str(args.student_results), 'corrections': [], 'acceptance_used': False}
    try:
        for start in range(0, len(wrong), 5):
            student_batch = wrong[start:start+5]
            batch = [by_id[r['id']] for r in student_batch]
            payload = [{'id': row['id'], 'context': row['prompt'], 'student_output': student['output']}
                       for row, student in zip(batch, student_batch)]
            chunks = []
            service.stream(json.dumps(payload, ensure_ascii=False),
                           TEACHER + '\nstudent_output 是小模型刚才的实际输出，可能有错。重新依据事实给出选择，不能迎合它；不要改人物性格或编造事件来让旧输出成立。',
                           threading.Event(), chunks.append, model='gpt-6.1-sol')
            corrected = parse_output(''.join(chunks), batch)
            for value, student in zip(corrected, student_batch):
                if not value['input_ok']:
                    raise ValueError('纠错发现无效输入: ' + value['id'])
                original = next(r for r in teacher['results'] if r['id'] == value['id'])
                audit['corrections'].append({'id': value['id'], 'student': student,
                                             'original_teacher': dict(original), 'teacher_correction': value})
                original.update(value)
            write_json(destination, audit)
            print(json.dumps({'corrected': len(audit['corrections'])}), flush=True)
    finally:
        service.close()
    write_json(args.folder / 'teacher-corrected.json', teacher)
    print(json.dumps(prepare_training(args.folder, rows, teacher['results']), ensure_ascii=False))


if __name__ == '__main__':
    main()
