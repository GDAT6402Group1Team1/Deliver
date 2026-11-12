"""用独立模型连接比较两版提示词，保存逐句台词、动作和用量，不修改正式后台"""
import argparse
import copy
import json
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

from app_server import AppServer
from dialogue_prompt_experiment import ROOT, build_bundle
from server import validate_action
from streaming import SentenceParser


CASES = ('ordinary_request', 'figurative_to_literal', 'road_correction', 'release_is_intention')
MODELS = ('gpt-6.1-sol', 'gpt-6-luna')


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--output', default='dialogue-prompt-first-comparison.json')
    args.add_argument('--check-only', action='store_true')
    args.add_argument('--persona-only', action='store_true', help='只用正式提示词，比较简化人物设定的独立一批')
    options = args.parse_args()
    profiles = json.loads((ROOT/'Design/AI/NpcProfiles.json').read_text(encoding='utf-8'))['characters']
    bundle = build_bundle(profiles)
    selected = [row for row in bundle['cases'] if row['case_id'] in CASES]
    assert len(profiles) == 2 and len(selected) == 8
    planned_calls = 16 if options.persona_only else 32
    if options.check_only:
        print(json.dumps({'planned_calls': planned_calls, 'persona_only': options.persona_only,
                          'cases': list(CASES), 'models': list(MODELS),
                          'instruction_sizes': bundle['instruction_sizes']}, ensure_ascii=False))
        return
    output = ROOT/'Saved/AIResearch'/Path(options.output).name
    if output.exists():
        raise FileExistsError('Choose a new output name to preserve the earlier comparison')
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {'started_utc': datetime.now(timezone.utc).isoformat(), 'status': 'running',
              'scope': f'{planned_calls} real model calls; no TTS, UE, or production backend connection',
              'studied_factor': 'persona_only' if options.persona_only else 'instructions',
              'effort': 'low (unchanged AppServer setting)',
              'models': list(MODELS), 'cases': selected,
              'instructions': bundle['instructions'], 'instruction_sizes': bundle['instruction_sizes'],
              'protocol_valid_meaning': 'Only JSON structure and original candidate/action validation; not dialogue quality',
              'results': [], 'manual_review': {'status': 'pending', 'rows': []}}

    def save():
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    service = AppServer()
    try:
        start = time.monotonic()
        service._start()
        report['process_startup_ms'] = round((time.monotonic()-start)*1000)
        save()
        for profile_index, profile in enumerate(profiles):
            for case_index, name in enumerate(CASES):
                case = next(row for row in selected if row['profile_id'] == profile['id'] and row['case_id'] == name)
                models = MODELS if (profile_index+case_index) % 2 == 0 else tuple(reversed(MODELS))
                for model_index, model in enumerate(models):
                    variants = ('current', 'short_v1')
                    if options.persona_only:
                        variants = ('current',)
                    if (profile_index+case_index+model_index) % 2:
                        variants = tuple(reversed(variants))
                    for variant in variants:
                        prompt = case['persona_variant_input'] if options.persona_only else case['model_input']
                        instructions = bundle['instructions'][variant]
                        row = {'index': len(report['results'])+1, 'model': model, 'variant': variant,
                               'persona_variant': 'candidate' if options.persona_only else 'original',
                               'profile_id': profile['id'], 'character': profile['display_name'], 'case': name,
                               'input_characters': len(prompt)+len(instructions),
                               'input_utf8_bytes': len(prompt.encode('utf-8'))+len(instructions.encode('utf-8')),
                               'speech': [], 'raw_output': '', 'events': [], 'connection_stages_ms': {},
                               'first_delta_ms': None, 'first_complete_sentence_ms': None}
                        started = time.monotonic()

                        def elapsed():
                            return round((time.monotonic()-started)*1000)

                        def receive(kind, value):
                            row['events'].append({'kind': kind, 'elapsed_ms': elapsed(), 'value': copy.deepcopy(value)})
                            if kind == 'sentence':
                                if row['first_complete_sentence_ms'] is None:
                                    row['first_complete_sentence_ms'] = elapsed()
                                row['speech'].append(value['text'])

                        parser = SentenceParser(case['context'], validate_action, receive)

                        def delta(text):
                            if text and row['first_delta_ms'] is None:
                                row['first_delta_ms'] = elapsed()
                            row['raw_output'] += text
                            parser.feed(text)

                        def measure(stage, milliseconds):
                            row['connection_stages_ms'][stage] = round(milliseconds)

                        try:
                            service.stream(prompt, instructions, threading.Event(), delta, model=model, measure=measure)
                            parser.finish()
                            row.update(protocol_valid=True, action=parser.proposal['action'],
                                       target=parser.proposal['target_id'], proposal=copy.deepcopy(parser.proposal))
                        except Exception as error:
                            row.update(protocol_valid=False, error=type(error).__name__)
                        row['complete_ms'] = elapsed()
                        row['token_usage'] = copy.deepcopy(getattr(service, 'last_token_usage', None))
                        report['results'].append(row)
                        save()
                        print(json.dumps({key: row.get(key) for key in ('index', 'character', 'case', 'model', 'variant',
                              'protocol_valid', 'first_delta_ms', 'first_complete_sentence_ms', 'complete_ms', 'speech', 'action')},
                              ensure_ascii=False), flush=True)
        report['status'] = 'completed'
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    finally:
        service.close()


if __name__ == '__main__':
    main()
