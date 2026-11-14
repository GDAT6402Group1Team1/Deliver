"""独立比较重复说明合并版，默认只准备文件；--run 才调用八次模型，不调用语音或游戏"""
import argparse
import hashlib
import json
from pathlib import Path
import threading
import time

from app_server import AppServer
from benchmark_app_server_isolation import validation_context
from companion_context import COMPANION_ACTIONS, companion_instructions
from server import validate_action
from streaming import SentenceParser, STREAM_INSTRUCTIONS

ROOT = Path(__file__).resolve().parents[2]


def companion_case():
    return {'name': 'companion_follow_to_stay', 'origin': 'constructed_protocol_2_context', 'context': {
        'persona': '你是牛二狗，年轻的面馆帮厨。玩家扶你起来后你认他作大哥，认真、忠心、憨厚，肯干活。笑点来自逞强和处境，不靠故意听错或固定口头禅。按主人的当前安排陪伴，只有游戏确认的事才能说已经做完。',
        'self_name': '牛二狗', 'self_subject_id': 'p1',
        'subjects': [{'subject_id': 'p0', 'kind': 'player', 'name': '快递员甲'},
                     {'subject_id': 'p1', 'kind': 'npc', 'name': '牛二狗'}],
        'companion_context': {'owner_name': '快递员甲', 'requester_is_owner': True, 'mode': 'Follow',
            'suspension_reason': '', 'command_revision': 8, 'task_id': 'Task_001', 'task_phase': 'Accepted'},
        'pending_player_message': {'event_id': 'e2', 'subject_id': 'p0', 'addressee_id': 'p1',
            'kind': 'PlayerSpeech', 'description': '你先在这等着，我拿完回来叫你'},
        'current_player_message': {'event_id': 'e2', 'has_complete_reply': False},
        'recent_self_speech_ids': [], 'last_complete_player_reply_id': 'e1', 'new_event_ids': ['e2'],
        'evidence': [
            {'event_id': 'e0', 'subject_id': 'p0', 'addressee_id': 'p1', 'kind': 'PlayerSpeech', 'source': 'Heard', 'description': '先跟着我去拿第一单'},
            {'event_id': 'e1', 'subject_id': 'p1', 'kind': 'ActionReply', 'source': 'ActionResult', 'description': 'Follow 安排已被服务器接受，正在跟随主人', 'reply_to_event_id': 'e0'},
            {'event_id': 'e2', 'subject_id': 'p0', 'addressee_id': 'p1', 'kind': 'PlayerSpeech', 'source': 'Heard', 'description': '你先在这等着，我拿完回来叫你'}],
        'candidates': [{'action': 'Wait', 'target_id': '', 'duration': 1, 'evidence_ids': []}] +
            [{'action': action, 'target_id': '', 'duration': 0, 'evidence_ids': ['e2']} for action in COMPANION_ACTIONS]}}


def merged_instructions(original):
    """只合并重复的记录语义和输出要求，保留人物、现场与所有动作说明"""
    changes = [
        ('pending_player_message 是', 'ActionFinished 的 Succeeded',
         'pending_player_message.description 是待回复的最新玩家原话；结合人物和新变化回应。current_player_message.has_complete_reply=true 时这条及更早发言已经回答，不再补答；event_id 不同就是新发言，即使文字相同\n'
         'evidence 按时间排列，Speech 是别人说的话，SelfSpeech 是你已经说完的内容或被打断前的部分。SelfSpeech/ActionReply 只有 reply_to_event_id 对应某次 PlayerSpeech 才表示那次已回应；无对应编号的独白、闲聊、半段回复，以及 ActionFinished 都不算完整回应\n'
         'recent_self_speech_ids 指向最近已说的话，last_complete_player_reply_id 指向上次完整回应记录，用于接着聊与避免重说\n'),
        ('new_event_ids 是', 'Wait 是原地停留',
         'new_event_ids 指向上次决策尚未看到的事件，包含生成或播放上一句时新听到的话；不能因自己后来才说完而漏看。旧请求没有这项时按发言完成时间估算\n'
         '没有待回复玩家时，只回应值得接话的新变化；旧对话用于理解经过，不重答旧事。再次看见同一人、动作结束不必逐条说话，已经说清且没有新事可安静等待。动作只选本轮 candidates\n'),
        ('输出 1 到 3 行 JSON', 'background 是作者',
         '输出 1–3 行完整 JSON 对象，不加其他文字。首行 {"candidate_index":0,"speech":"一句自然完整的回应。","emotion":"Calm","speech_target_id":"收话人的编号"}；candidate_index 是候选下标，emotion 只允许 Calm/Angry/Surprised；subjects 缺省时按前述规则省略 speech_target_id\n'
         '首行决定全轮唯一动作与目标，后续不得改变。每行说一句完整自然的口语，写完立即换行供合成；后续行只有 {"speech":"下一句。"}，有新意思才继续，别切碎连贯的一句话或先挤几个字充数\n'
         '全轮最多3句，每句最多32个中文字，总共最多90字；没有值得说的新话时首行 speech 为空并结束，不能追加后续行。只有意图，不能声称尚未确认的动作已完成、打中或拽住\n'),
    ]
    result = original
    for start, stop, replacement in changes:
        if result.count(start) != 1 or result.count(stop) != 1:
            raise ValueError('prompt_changed_review_merge_again')
        begin, end = result.index(start), result.index(stop)
        if begin >= end:
            raise ValueError('invalid_prompt_section_order')
        result = result[:begin] + replacement + result[end:]
    return result


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument('--run', action='store_true')
    arguments.add_argument('--output', default='prompt-deduplication-20261114.json')
    args = arguments.parse_args()
    ids = ('7b38d587-44a5-cef8-9cc0-9db9c5a40b9c', '1de8ec30-42f8-d4a1-d5dc-59a2dbe1f391')
    records = [json.loads(line) for line in (ROOT/'Saved/Logs/NpcDialogue.ndjson').read_text(encoding='utf-8').splitlines()]
    by_id = {row['request_id']: row for row in records if row.get('request_id') in ids and 'model_context' in row}
    cases = [{'name': key, 'context': by_id[key]['model_context']} for key in ids]
    prepared = json.loads((ROOT/'Saved/AIResearch/dialogue-prompt-experiment.json').read_text(encoding='utf-8'))
    for name in ('road_correction',):
        source = next(row for row in prepared['cases'] if row['case_id'] == name)
        cases.append({'name': name, 'context': json.loads(source['model_input']), 'review': source['review']})
    cases.append(companion_case())
    variants = {'current': STREAM_INSTRUCTIONS, 'merged': merged_instructions(STREAM_INSTRUCTIONS)}
    report = {'model': 'gpt-6.1-sol', 'effort': 'low', 'cases': cases, 'instructions': variants,
              'instruction_sizes': {key: len(value) for key, value in variants.items()},
              'instruction_hashes': {key: hashlib.sha256(value.encode('utf-8')).hexdigest() for key, value in variants.items()},
              'status': 'prepared', 'results': []}
    for case in cases:
        context, _ = validation_context(case['context'])
        for candidate in context['candidates']:
            validate_action(context, {**candidate, 'speech': '', 'emotion': 'Calm'})
    output = ROOT/'Saved/AIResearch'/Path(args.output).name
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'status': 'prepared', 'sizes': report['instruction_sizes'], 'calls': 8 if args.run else 0}), flush=True)
    if not args.run:
        return
    service = AppServer()
    try:
        for index, case in enumerate(cases):
            prompt = json.dumps(case['context'], ensure_ascii=False, separators=(',', ':'))
            order = ('current', 'merged') if index % 2 == 0 else ('merged', 'current')
            for variant in order:
                instructions = variants[variant] + companion_instructions(case['context'])
                row = {'case': case['name'], 'variant': variant, 'speech': [], 'stages_ms': {},
                       'prompt_sha256': hashlib.sha256(prompt.encode('utf-8')).hexdigest(),
                       'instructions_sha256': hashlib.sha256(instructions.encode('utf-8')).hexdigest()}
                context, targets = validation_context(case['context'])
                started = time.monotonic()

                def receive(kind, value):
                    if kind == 'proposal':
                        row['decision_ms'] = round((time.monotonic()-started)*1000)
                    elif kind == 'sentence':
                        row.setdefault('first_sentence_ms', round((time.monotonic()-started)*1000))
                        row['speech'].append(value['text'])

                reader = SentenceParser(context, validate_action, receive)
                reader.speech_targets = targets
                try:
                    service.stream(prompt, instructions, threading.Event(), reader.feed, model='gpt-6.1-sol',
                                   measure=lambda stage, elapsed: row['stages_ms'].update({stage: round(elapsed)}))
                    reader.finish()
                    chosen = next(i for i, candidate in enumerate(context['candidates']) if
                                  candidate['action'] == reader.proposal['action'] and candidate['target_id'] == reader.proposal['target_id'])
                    row.update(valid=True, action=reader.proposal['action'], target=case['context']['candidates'][chosen]['target_id'],
                               duration=reader.proposal['duration'],
                               speech_target=next((short for short, raw in targets.items() if raw == reader.proposal.get('speech_target_id')), None),
                               emotion=reader.proposal['emotion'])
                except Exception as error:
                    row.update(valid=False, error=type(error).__name__)
                row['usage'] = service.last_token_usage
                row['complete_ms'] = round((time.monotonic()-started)*1000)
                report['results'].append(row)
                output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
                print(json.dumps(row, ensure_ascii=True), flush=True)
        report['status'] = 'complete'
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    finally:
        service.close()


if __name__ == '__main__':
    main()
