"""真实模型打断后比较保留进程与重新启动，不连接游戏或正式后端"""
import argparse
import json
import threading
import time
from pathlib import Path

from app_server import AppServer
from review_dialogue import make_cases
from server import validate_action
from streaming import SentenceParser, STREAM_INSTRUCTIONS, model_context

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rounds', type=int, default=2)
    parser.add_argument('--output', default='model-cancel-reuse-comparison.json')
    args = parser.parse_args()
    profile = json.loads((ROOT / 'Design/AI/NpcProfiles.json').read_text(encoding='utf-8'))['characters'][0]
    results = []
    output = ROOT / 'Saved/AIResearch' / Path(args.output).name
    output.parent.mkdir(parents=True, exist_ok=True)
    for repeat in range(args.rounds):
        policies = ['keep_confirmed', 'restart_confirmed']
        if repeat % 2:
            policies.reverse()
        for policy in policies:
            service = AppServer()
            row = {'policy': policy, 'round': repeat, 'cancel_stages': {}, 'next_stages': {}, 'speech': []}
            cancel = threading.Event()
            cancelled_at = None
            context = make_cases(profile)[0][1]
            try:
                service._start()
                before_pid = service.process.pid

                def interrupt(text):
                    nonlocal cancelled_at
                    if text and cancelled_at is None:
                        cancelled_at = time.monotonic()
                        cancel.set()

                try:
                    service.stream(json.dumps(model_context(context), ensure_ascii=False), STREAM_INSTRUCTIONS,
                                   cancel, interrupt, model='gpt-6.1-sol',
                                   measure=lambda stage, ms: row['cancel_stages'].update({stage: round(ms)}))
                    row['cancel_result'] = 'completed'
                except RuntimeError as error:
                    row['cancel_result'] = str(error)
                row['cancel_ms'] = round((time.monotonic()-cancelled_at)*1000) if cancelled_at else None
                row['kept_after_confirmation'] = bool(service.process and service.process.poll() is None and service.process.pid == before_pid)
                if policy == 'restart_confirmed':
                    service.close()
                next_context = dict(make_cases(profile))['interrupted_reply']
                started = time.monotonic()

                def receive(kind, value):
                    if kind == 'sentence':
                        row.setdefault('next_sentence_ms', round((time.monotonic()-started)*1000))
                        row['speech'].append(value['text'])

                reader = SentenceParser(next_context, validate_action, receive)
                service.stream(json.dumps(model_context(next_context), ensure_ascii=False), STREAM_INSTRUCTIONS,
                               threading.Event(), reader.feed, model='gpt-6.1-sol',
                               measure=lambda stage, ms: row['next_stages'].update({stage: round(ms)}))
                reader.finish()
                row['same_process_for_next'] = bool(service.process and service.process.pid == before_pid)
                row['next_valid'] = True
            except Exception as error:
                row['error'] = type(error).__name__
            finally:
                service.close()
            results.append(row)
            output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
            print(json.dumps(row, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
