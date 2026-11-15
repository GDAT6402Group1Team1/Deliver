"""检查已运行的小模型接口，默认只显示计划，--run 才发送最多三次模型请求"""
import argparse
from copy import deepcopy
from datetime import datetime, timezone
import http.client
import json
import os
from pathlib import Path
import sys
import threading
import time
import uuid
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from combat_model import LocalCombatClient, choose_combat_action, _endpoint
from server import validate_context
from prepare_local_model import INSTALL, ROOT, file_hash
from start_local_model import existing_process


def identifier(label):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, 'delivery-combat-integration-probe/' + label))


def request(label):
    """只构造技术检查情境，不读取训练样本或验收集，也不指定模型必须选哪种行动"""
    own, target, event = (identifier(name) for name in ('self', 'opponent', 'event'))
    data = {'protocol_version': 1, 'capability_version': 4, 'stream_protocol_version': 2,
            'world_epoch': identifier('world'), 'npc_id': own, 'request_id': identifier(label),
            'context_revision': 2, 'plan_revision': 3, 'profile_id': 'chunlai',
            'persona': '大壮是住在村口的老街坊，嘴快、好面子，真挨疼也会躲。只根据自己听到和看到的事情判断。',
            'self_name': '大壮', 'self_subject_id': own,
            'request_source': 'ambient', 'pending_player_message': False,
            'subjects': [{'subject_id': own, 'name': '大壮', 'kind': 'npc'},
                         {'subject_id': target, 'name': '翠花', 'kind': 'npc'}],
            'evidence': [{'event_id': event, 'subject_id': target, 'addressee_id': own,
                          'kind': 'Speech', 'description': '你刚才撞了我的篮子，先把东西捡起来！'},
                         {'event_id': identifier('body'), 'subject_id': own, 'addressee_id': '',
                          'kind': 'SelfCombat', 'description': '我还站着，刚才没有被击倒；对方离我约两米。'}],
            'allowed_actions': ['Fight', 'Flee', 'Wait'],
            'candidates': [{'action': action, 'target_id': '' if action == 'Wait' else target,
                            'duration': 1. if action == 'Wait' else 4.,
                            'evidence_ids': [] if action == 'Wait' else [event]}
                           for action in ('Fight', 'Flee', 'Wait')],
            'current_action': {'action': 'Fight', 'action_id': identifier('running'),
                               'target_id': target, 'source': 'Model', 'remaining_seconds': 3.5}}
    validate_context(data)
    return data


def excluded_requests():
    player = request('player-continue')
    player.update(capability_version=6, request_source='player', pending_player_message=True)
    player['current_action']['plan_revision'] = 2
    for index, candidate in enumerate(player['candidates']):
        candidate['candidate_id'] = identifier('candidate-' + str(index))
    player['allowed_actions'].append('ContinueCurrent')
    player['candidates'].append({'candidate_id': identifier('continue'), 'action': 'ContinueCurrent',
                                 'target_id': '', 'duration': 0, 'evidence_ids': []})
    companion = request('companion-exclusion')
    companion.update(capability_version=2, profile_id='ergou', self_name='牛二狗', persona='认真干活的年轻帮厨')
    companion['subjects'][0]['name'] = '牛二狗'
    companion['companion_context'] = {'owner_name': '快递员甲', 'requester_is_owner': False,
        'mode': 'Follow', 'suspension_reason': '', 'command_revision': 2, 'task_id': '', 'task_phase': ''}
    for value in (player, companion):
        validate_context(value)
    return [('v6_player_continue', player, 'player_or_unknown_request'),
            ('companion', companion, 'companion_not_in_training_scope')]


def check_exclusions():
    class NoModelCalls:
        def __init__(self):
            self.calls = 0
        def choose(self, *_args):
            self.calls += 1
            raise AssertionError('excluded_request_reached_model')
    rows = []
    for name, context, expected in excluded_requests():
        client = NoModelCalls()
        result = choose_combat_action(context, threading.Event(), client=client)
        rows.append({'name': name, 'method': 'routing_only_guard_client_not_real_inference',
                     'passed': client.calls == 0 and result.proposal is None and result.fallback_reason == expected,
                     'model_calls': client.calls, 'fallback_reason': result.fallback_reason})
    return rows


def read_slots(endpoint, slot_count=1):
    host, port = _endpoint(endpoint)
    connection = http.client.HTTPConnection(host, port, timeout=.5)
    try:
        connection.request('GET', '/slots')
        response = connection.getresponse()
        raw = response.read(65537)
        if response.status != 200 or len(raw) > 65536:
            raise ValueError('slots_unavailable')
        slots = json.loads(raw)
        if (not isinstance(slots, list) or len(slots) != slot_count
                or not all(isinstance(slot, dict) and type(slot.get('is_processing')) is bool for slot in slots)
                or {slot.get('id') for slot in slots} != set(range(slot_count))):
            raise ValueError('unexpected_slots')
        return any(slot['is_processing'] for slot in slots)
    finally:
        connection.close()


def wait_for_idle(endpoint, seconds=8., slot_count=1):
    started = time.perf_counter()
    while time.perf_counter() - started < seconds:
        if not read_slots(endpoint, slot_count):
            return round((time.perf_counter() - started) * 1000, 2)
        time.sleep(.02)
    raise TimeoutError('local_model_still_processing')


def run_choice(endpoint, label, *, compact_output=False, resident_cache=False):
    context = request(label)
    if resident_cache and label == 'after_cancel':
        context.update(profile_id='xiumei', self_name='翠花', persona='翠花是村口老街坊，脾气急，真打疼了会先躲开。')
        context['subjects'][0]['name'], context['subjects'][1]['name'] = '翠花', '大壮'
        validate_context(context)
    before = deepcopy(context)
    client = None if resident_cache else LocalCombatClient(endpoint, timeout_seconds=1.5, seed=20261114,
                                                             compact_output=compact_output)
    stages = []
    started = time.perf_counter()
    result = choose_combat_action(context, threading.Event(),
        lambda stage, ms: stages.append({'stage': stage, 'elapsed_ms': round(ms, 2)}), client=client)
    proposal = result.proposal
    offered = proposal and next((candidate for candidate in context['candidates']
                                if candidate['action'] == proposal['action']), None)
    valid = bool(offered and all(proposal[key] == offered[key]
                                for key in ('action', 'target_id', 'evidence_ids', 'duration')) and
                 proposal['request_id'] == context['request_id'] and proposal['world_epoch'] == context['world_epoch'] and
                 proposal['npc_id'] == context['npc_id'] and proposal['speech'] == '' and
                 proposal['speech_target_id'] == '' and context == before)
    return {'name': label, 'method': 'real_model_http', 'passed': valid,
            'elapsed_ms': round((time.perf_counter() - started) * 1000, 2),
            'context': context, 'proposal': proposal, 'fallback_reason': result.fallback_reason,
            'raw_model_output': client.last_output if client else None,
            'usage': client.last_usage if client else None, 'stages': stages}


def check_cancellation(endpoint, *, compact_output=False, resident_cache=False):
    context = request('cancel-active-inference')
    client = None if resident_cache else LocalCombatClient(endpoint, timeout_seconds=1.5, seed=20261114,
                                                             compact_output=compact_output)
    slot_count = 2 if resident_cache else 1
    cancel, completed = threading.Event(), threading.Event()
    started = time.perf_counter()
    results = []
    stages = []

    def call():
        try:
            results.append(choose_combat_action(context, cancel,
                lambda stage, ms: stages.append({'stage': stage, 'elapsed_ms': round(ms, 2)}), client=client))
        finally:
            completed.set()

    worker = threading.Thread(target=call, name='CombatIntegrationProbe', daemon=True)
    worker.start()
    cancelled_at = None
    observed_processing = False
    try:
        while not completed.is_set() and time.perf_counter() - started < 2:
            if read_slots(endpoint, slot_count):
                observed_processing = True
                if not completed.is_set():
                    cancelled_at = time.perf_counter()
                    cancel.set()
                break
            completed.wait(.01)
        worker.join(2)
        if worker.is_alive():
            raise TimeoutError('cancelled_call_not_finished')
        confirmed_at = time.perf_counter()
        if not results:
            raise RuntimeError('missing_cancel_result')
        result = results[0]
        # 服务计算太快、尚未看见忙碌状态时就已结束，只报告未覆盖，不冒充取消成功
        covered = observed_processing and cancelled_at is not None
        passed = covered and result.proposal is None and result.fallback_reason == 'cancelled'
        idle_ms = wait_for_idle(endpoint, slot_count=slot_count)
        # 生产适配器每 0.2 秒检查一次空闲，留出一次检查时间再提交后续请求
        time.sleep(.25)
        return {'name': 'cancel_active_inference', 'method': 'real_model_http_and_slots',
                'status': 'passed' if passed else 'failed' if covered else 'not_covered',
                'passed': passed, 'observed_service_processing': observed_processing,
                'context': context, 'proposal_after_cancel': result.proposal if covered else None,
                'uncancelled_result': result.proposal if not covered else None,
                'fallback_reason': result.fallback_reason,
                'cancel_latency_ms': None if cancelled_at is None else round((confirmed_at - cancelled_at) * 1000, 2),
                'service_idle_after_return_ms': idle_ms, 'stages': stages}
    finally:
        cancel.set()
        worker.join(2)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--compact-output', action='store_true', help='检查动作名输出及其取消流程')
    parser.add_argument('--resident-cache', action='store_true', help='通过游戏后端配置检查两位街坊的缓存与取消')
    parser.add_argument('--runtime-record', type=Path, default=INSTALL / 'running.json')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    if args.resident_cache and not args.compact_output:
        parser.error('--resident-cache 需要 --compact-output')
    slot_count = 2 if args.resident_cache else 1
    contexts = [request(name) for name in ('target_mapping', 'cancel-active-inference', 'after_cancel')]
    sources = [Path(__file__).resolve(), *[Path(__file__).resolve().parents[1] / name
                for name in ('combat_model.py', 'server.py', 'streaming.py', 'continued_activity.py')]]
    plan = {'mode': 'run' if args.run else 'dryrun', 'maximum_model_requests': 3,
            'output_mode': 'action_name' if args.compact_output else 'json',
            'resident_cache': args.resident_cache, 'slot_count': slot_count,
            'runtime_record': str(args.runtime_record), 'contexts': contexts,
            'source_hashes': {path.relative_to(ROOT).as_posix(): file_hash(path) for path in sources},
            'routing_checks': check_exclusions(),
            'scope': '技术接口检查，不评价剧情选择，不运行语音或UE，不读取冻结验收标签'}
    if not args.run:
        print(json.dumps(plan, ensure_ascii=False, indent=2))
        return 0
    output = (args.output or ROOT / 'Saved/AIResearch/NpcTraining/combat-v6-integration' /
              ('real-probe-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))).resolve()
    output.relative_to((ROOT / 'Saved').resolve())
    output.mkdir(parents=True, exist_ok=True)
    report = {**plan, 'status': 'running', 'started_utc': datetime.now(timezone.utc).isoformat(),
              'passed': False, 'ue_integration_verified': False,
              'limitations': ['Windows 等待响应头时取消可能到1.5秒配置截止才返回',
                              '目标映射只检查模型实际选择的动作，不把合法选择当作行为质量通过',
                              'ContinueCurrent与同伴排除只检查路由，不调用模型',
                              '没有游戏动作、声音播放、双人或GPU竞争检查'], 'checks': []}
    code = 1
    try:
        runtime = json.loads(args.runtime_record.read_text(encoding='utf-8'))
        command = runtime['command']
        host = command[command.index('--host') + 1]
        port = command[command.index('--port') + 1]
        endpoint = f'http://{host}:{port}'
        _endpoint(endpoint)
        if host != '127.0.0.1' or runtime['health'] != endpoint + '/health' or runtime['slots'] != endpoint + '/slots':
            raise ValueError('runtime_endpoint_mismatch')
        model = Path(runtime['model_path'])
        if file_hash(model) != runtime['model_sha256']:
            raise ValueError('runtime_model_hash_mismatch')
        if not existing_process(args.runtime_record, command, runtime['model_sha256'], slot_count=slot_count):
            raise ValueError('runtime_process_does_not_match_record')
        if read_slots(endpoint, slot_count):
            raise RuntimeError('service_busy_before_probe')
        installation = json.loads((INSTALL / 'installation.json').read_text(encoding='utf-8'))
        report['runtime'] = runtime
        report['runtime_hashes'] = {'record': file_hash(args.runtime_record), 'model': runtime['model_sha256'],
            'executable': file_hash(Path(command[0])), 'installation': file_hash(INSTALL / 'installation.json'),
            'dependencies': {row['path']: file_hash(INSTALL / row['path']) for row in installation['runtime_files']}}
        options = {'compact_output': args.compact_output, 'resident_cache': args.resident_cache}
        setup = {'DELIVERY_COMBAT_MODEL_URL': endpoint, 'DELIVERY_COMBAT_MODEL_TIMEOUT_SECONDS': '1.5',
                 'DELIVERY_COMBAT_MODEL_TEMPERATURE': '0', 'DELIVERY_COMBAT_MODEL_PROFILE_IDS': 'chunlai,xiumei',
                 'DELIVERY_COMBAT_MODEL_RESIDENT_CACHE': '1' if args.resident_cache else '0'}
        with patch.dict(os.environ, setup):
            report['checks'].append(run_choice(endpoint, 'target_mapping', **options))
            if not report['checks'][-1]['passed']:
                raise RuntimeError('target_mapping_check_failed')
            wait_for_idle(endpoint, slot_count=slot_count)
            report['checks'].append(check_cancellation(endpoint, **options))
            report['checks'].append(run_choice(endpoint, 'after_cancel', **options))
        report['passed'] = all(row['passed'] for row in [*report['routing_checks'], *report['checks']])
        report['status'] = 'passed' if report['passed'] else 'not_covered' if report['checks'][1]['status'] == 'not_covered' else 'failed'
        code = 0 if report['passed'] else 2
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        path = output / 'probe.json'
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({'status': report['status'], 'report': str(path), 'sha256': file_hash(path)}, ensure_ascii=False))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
