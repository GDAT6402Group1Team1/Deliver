"""在已运行的决策服务旁测一份原生 B 语音，默认只检查配置，加 --run 才运行

封存的 40 条情境只用于评测，标签不会发给模型，也不生成训练数据
READY 后留出时间启动 UE CSV 录制，本脚本不操作游戏界面
"""
import argparse
from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'Tools/DeliveryAI'))
from app_server import AppServer
from combat_model import COMBAT_INSTRUCTIONS, LocalCombatClient, choose_combat_action, parse_combat_choice, prepare_combat_context
from native_tts import DEFAULT_RUNTIME, NativeQwenSpeechSynthesizer
from tts_evaluation.compare_native_qwen import GpuSamples, LINES, command_output, content_hash
from tts_evaluation.probe_http_speech import buffering_metrics

EVALUATION_PROFILES = {'大壮': 'chunlai', '翠花': 'xiumei'}


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def load_cases(path):
    source = path.read_bytes()
    payload = json.loads(source.decode('utf-8-sig'))
    rows = payload['cases'] if isinstance(payload, dict) else payload
    if not isinstance(rows, list) or len(rows) != 40 or len({row['id'] for row in rows}) != 40:
        raise ValueError('需要 40 条不同编号的封存情境')
    for row in rows:
        if row.get('usage') != 'acceptance_only':
            raise ValueError('只接收 acceptance_only 数据，不接受训练样本')
        _, candidates = prepare_combat_context(row['context'])
        acceptable = row.get('label', {}).get('acceptable_actions')
        if not isinstance(acceptable, list) or not acceptable or any(action not in candidates for action in acceptable):
            raise ValueError('评测标签与候选不符：'+row['id'])
    return rows, hashlib.sha256(source).hexdigest()


def evaluation_context(context):
    """旧评测没有人物配置编号，只给副本补上正式编号，原始情境和模型输入保持不变"""
    copied = deepcopy(context)
    if not copied.get('profile_id') and 'companion_context' not in copied:
        profile = EVALUATION_PROFILES.get(copied.get('self_name'))
        if profile:
            copied['profile_id'] = profile
    return copied


class LunaCombatClient:
    """云端与本地使用相同的语义输入，只接受动作和目标，不生成台词"""
    def __init__(self, timeout_seconds, model='gpt-6-luna'):
        self.timeout_seconds, self.model = timeout_seconds, model
        self.service = AppServer()
        self.last_output, self.last_usage = '', None

    def choose(self, prompt, candidates, cancel, measure=None):
        self.last_output, self.last_usage = '', None
        request_cancel, finished = threading.Event(), threading.Event()
        deadline = time.monotonic()+self.timeout_seconds

        def watch():
            while not finished.wait(.02):
                if cancel.is_set() or time.monotonic() >= deadline:
                    request_cancel.set()
                    return

        watcher = threading.Thread(target=watch, name='coexist-luna-cancel', daemon=True)
        watcher.start()
        parts = []
        try:
            self.service.stream(json.dumps(prompt, ensure_ascii=False, separators=(',', ':')),
                                COMBAT_INSTRUCTIONS, request_cancel, parts.append, model=self.model, measure=measure)
            if cancel.is_set():
                raise RuntimeError('cancelled')
            if time.monotonic() >= deadline:
                raise TimeoutError('combat_model_timeout')
            return parse_combat_choice(''.join(parts), candidates)
        except Exception:
            if not cancel.is_set() and time.monotonic() >= deadline:
                raise TimeoutError('combat_model_timeout')
            raise
        finally:
            self.last_output = ''.join(parts)
            self.last_usage = self.service.last_token_usage
            finished.set()
            watcher.join(.2)

    def close(self):
        self.service.close()


def warmup_model(client, report, timeout_seconds=None):
    """单独记录冷预热，结束后恢复正式请求的时限"""
    request_timeout = client.timeout_seconds
    warmup_timeout = request_timeout if timeout_seconds is None else timeout_seconds
    if not math.isfinite(warmup_timeout) or not .1 <= warmup_timeout <= 30:
        raise ValueError('预热限时需为 0.1–30 秒')
    # 预热使用额外场景，不读取封存情境的答案，也不提前缓存第一道评测题
    prompt = {'self_name': '预热角色', 'persona': '怕疼，愿意停手', 'subjects': [],
              'current_action': {'action': 'Fight', 'target': 'opponent', 'remaining_seconds': 3},
              'candidates': ['Fight', 'Flee', 'Wait'],
              'events': [{'kind': 'Speech', 'subject': 'opponent', 'description': '刚才是我不好，我们停手吧。'}]}
    began = time.monotonic()
    report.update(warmup_timeout_seconds=warmup_timeout, warmup_model_started_utc=utc_now())
    client.timeout_seconds = warmup_timeout
    try:
        report['warmup_model_action'] = client.choose(prompt, {key: {} for key in ('Fight', 'Flee', 'Wait')}, threading.Event())
        report['warmup_model_status'] = 'completed'
    except BaseException as error:
        report.update(warmup_model_status='failed', warmup_model_error=type(error).__name__+':'+str(error))
        raise
    finally:
        client.timeout_seconds = request_timeout
        report['warmup_model_ms'] = (time.monotonic()-began)*1000
        report['warmup_model_finished_utc'] = utc_now()


def synthesize_line(engine, line, cancel, chunk_frames=8, start_buffer=1.28):
    name, voice, text = line
    started, start_utc = time.monotonic(), utc_now()
    chunks, digest, total_bytes = [], hashlib.sha256(), 0

    def receive(pcm):
        nonlocal total_bytes
        if not pcm or len(pcm) % 2:
            raise ValueError('语音块为空或不是 16 位 PCM')
        chunks.append({'received_ms': (time.monotonic()-started)*1000,
                       'duration_s': len(pcm)/48000, 'bytes': len(pcm), 'offset_bytes': total_bytes})
        total_bytes += len(pcm)
        digest.update(pcm)

    row = {'case': name, 'voice': voice, 'text': text, 'utc_started': start_utc, 'status': 'completed'}
    try:
        size, rate = engine.synthesize_stream(text, voice, 'Calm', cancel, receive, chunk_frames=chunk_frames)
        if size != total_bytes or not total_bytes or rate != 24000:
            raise ValueError('语音结束长度或采样率不符')
    except Exception as error:
        row.update(status='cancelled' if cancel.is_set() else 'failed', error=str(error))
    elapsed = (time.monotonic()-started)*1000
    duration = total_bytes/48000
    simulation = buffering_metrics(chunks, elapsed, start_buffer=start_buffer) if row['status'] == 'completed' else None
    if simulation:
        simulation = {key.replace('buffer_064_', 'buffer_'): value for key, value in simulation.items()}
        simulation['start_buffer_s'] = start_buffer
    row.update(utc_finished=utc_now(), complete_ms=elapsed, chunks=chunks, chunk_count=len(chunks),
               first_chunk_ms=chunks[0]['received_ms'] if chunks else None, delivered_audio_seconds=duration,
               generation_rtf=elapsed/1000/duration if duration and row['status'] == 'completed' else None,
               pcm_sha256=digest.hexdigest(), buffer_simulation=simulation,
               transport='direct_worker_callback', ue_playback_measured=False)
    return row


def evaluate_choice(case, result, raw):
    proposal = result.proposal
    illegal_target = False
    try:
        encoded = json.loads(raw)
        if isinstance(encoded, dict) and encoded.get('action') in ('Fight', 'Flee', 'Wait'):
            illegal_target = encoded.get('target_id') != ('' if encoded['action'] == 'Wait' else 'opponent')
    except (ValueError, TypeError):
        pass
    if proposal:
        expected_target = '' if proposal['action'] == 'Wait' else case['context']['current_action']['target_id']
        illegal_target |= proposal['target_id'] != expected_target
    acceptable = case['label']['acceptable_actions']
    return {'action': proposal['action'] if proposal else None, 'target_id': proposal['target_id'] if proposal else None,
            'protocol_valid': proposal is not None and not illegal_target, 'illegal_target': illegal_target,
            'acceptable_actions': acceptable, 'matches_evaluation_label': bool(proposal and proposal['action'] in acceptable),
            'fallback_reason': result.fallback_reason, 'output': raw}


def run_window(engine, client, cases, cadence, duration, *, chunk_frames=8, start_buffer=1.28,
               decision_interval=1., stop=None):
    """模型按固定时刻发起，前一次没结束就跳过该时刻；下一次仍处理原来的下一道题"""
    stop = stop or threading.Event()
    start = time.monotonic()
    report = {'utc_started': utc_now(), 'requested_seconds': duration, 'decisions': [], 'speech': [], 'errors': [],
              'decision_schedule': {'interval_seconds': decision_interval, 'enabled': client is not None,
                                    'missed_slots': 0, 'actual_requests': 0}}
    print(json.dumps({'event': 'MEASUREMENT_STARTED', 'utc': report['utc_started'], 'seconds': duration}), flush=True)

    def decisions():
        index, attempts, slot = 0, 0, 0
        planned_slots = math.ceil(duration/decision_interval)
        while slot < planned_slots and not stop.wait(max(0., start+slot*decision_interval-time.monotonic())):
            case = cases[index % len(cases)]
            timing = {}
            started, timestamp = time.monotonic(), utc_now()
            client.last_output, client.last_usage = '', None
            routed_context = evaluation_context(case['context'])
            result = choose_combat_action(routed_context, stop, lambda stage, ms: timing.update({stage: ms}), client=client)
            elapsed = (time.monotonic()-started)*1000
            row = {'id': case['id'], 'case_index': index % len(cases), 'cycle': index//len(cases),
                   'attempt': attempts, 'utc_started': timestamp, 'utc_finished': utc_now(),
                   'scheduled_slot': slot, 'scheduled_at_seconds': slot*decision_interval,
                   'start_late_ms': max(0., started-start-slot*decision_interval)*1000,
                   'evaluation_profile_id': routed_context.get('profile_id'),
                   'profile_added_for_evaluation': routed_context.get('profile_id') != case['context'].get('profile_id'),
                   'started_at_seconds': started-start, 'complete_ms': elapsed, 'timing': timing,
                   'completed_inside_window': time.monotonic()-start <= duration,
                   'usage': getattr(client, 'last_usage', None), **evaluate_choice(case, result, client.last_output)}
            report['decisions'].append(row)
            attempts += 1
            if result.fallback_reason != 'busy':
                report['decision_schedule']['actual_requests'] += 1
            # 慢请求结束后不补发已经错过的请求，避免突然挤占显卡
            next_slot = max(slot+1, math.ceil((time.monotonic()-start)/decision_interval))
            missed = max(0, min(next_slot, planned_slots)-(slot+1))
            row['missed_slots_after_request'] = missed
            report['decision_schedule']['missed_slots'] += missed
            if result.fallback_reason == 'cancelled':
                break
            slot = next_slot
            if result.fallback_reason != 'busy':
                index += 1

    def speech():
        index, due = 0, 0.
        while not stop.wait(max(0., start+due-time.monotonic())):
            line = LINES[index % len(LINES)]
            started = time.monotonic()-start
            row = synthesize_line(engine, line, stop, chunk_frames, start_buffer)
            row.update(cycle=index//len(LINES), scheduled_at_seconds=due, started_at_seconds=started,
                       start_late_ms=max(0., started-due)*1000)
            report['speech'].append(row)
            if row['status'] == 'failed':
                raise RuntimeError('语音合成失败：'+row.get('error', 'unknown'))
            index += 1
            due += cadence[(index-1) % len(cadence)]['interval_seconds']

    def guarded(function):
        try:
            function()
        except BaseException as error:
            report['errors'].append(type(error).__name__+':'+str(error))
            stop.set()

    workers = [threading.Thread(target=guarded, args=(speech,), name='coexist-one-tts', daemon=True)]
    if client is not None:
        workers.append(threading.Thread(target=guarded, args=(decisions,), name='coexist-decisions', daemon=True))
    for worker in workers:
        worker.start()
    try:
        stop.wait(max(0., start+duration-time.monotonic()))
    except KeyboardInterrupt:
        report['errors'].append('operator_interrupted')
    finally:
        stop.set()
        report.update(utc_finished=utc_now(), actual_seconds=time.monotonic()-start)
        print(json.dumps({'event': 'MEASUREMENT_ENDED', 'utc': report['utc_finished']}, ensure_ascii=False), flush=True)
        for worker in workers:
            worker.join(45)
        if any(worker.is_alive() for worker in workers):
            report['errors'].append('worker_did_not_stop')
        report['cleanup_finished_utc'] = utc_now()
    return report


def process_snapshot():
    if os.name != 'nt':
        return {'supported': False, 'note': '此检查记录 Windows 进程，不据此判断 PIE 是否开启'}
    return {'ue': command_output(['tasklist', '/FI', 'IMAGENAME eq UnrealEditor.exe', '/FO', 'CSV', '/NH']),
            'ue_commandlet': command_output(['tasklist', '/FI', 'IMAGENAME eq UnrealEditor-Cmd.exe', '/FO', 'CSV', '/NH']),
            'local_model': command_output(['tasklist', '/FI', 'IMAGENAME eq llama-server.exe', '/FO', 'CSV', '/NH'])}


def reject_existing_tts():
    if os.name != 'nt':
        return
    command = ("@(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'python*' -and "
               "($_.CommandLine -match 'neural_tts_worker.py|native_tts_worker.py') } | "
               "Select-Object ProcessId,Name) | ConvertTo-Json -Compress")
    found = json.loads(command_output(['powershell', '-NoProfile', '-Command', command]) or '[]')
    if found:
        raise RuntimeError('已有语音工作进程；请先由测试协调者停止，避免同时加载两份 TTS')


def summary(window):
    calls = [row for row in window['decisions'] if row['fallback_reason'] != 'busy']
    completed = [row for row in calls if row['fallback_reason'] != 'cancelled']
    valid = [row for row in completed if row['protocol_valid']]
    durations = [row['complete_ms'] for row in completed]
    voices = [row for row in window['speech'] if row['status'] == 'completed']
    from llm_evaluation.analyze_game_frames import percentile
    return {'attempted_unique_cases': len({row['id'] for row in calls}),
            'completed_unique_cases': len({row['id'] for row in valid}),
            'all_40_cases_completed': len({row['id'] for row in valid}) == 40,
            'protocol_valid_calls': sum(row['protocol_valid'] for row in completed),
            'matched_label_calls': sum(row['matches_evaluation_label'] for row in completed),
            'illegal_target_calls': sum(row['illegal_target'] for row in completed),
            'complete_ms_median': statistics.median(durations) if durations else None,
            'complete_ms_p95': percentile(durations, .95) if durations else None,
            'speech_completed': len(voices), 'speech_partial_or_failed': len(window['speech'])-len(voices),
            'rtf_median': statistics.median(row['generation_rtf'] for row in voices) if voices else None,
            'simulated_stall_ms': sum(row['buffer_simulation']['simulated_total_stall_ms'] for row in voices),
            'quality_acceptance_claimed': False, 'ue_audio_measured': False}


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--provider', choices=('none', 'local', 'luna'), required=True)
    parser.add_argument('--source', type=Path, default=ROOT/'Saved/AIResearch/NpcTraining/acceptance-reviewed.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--endpoint', default='http://127.0.0.1:8771')
    parser.add_argument('--compact-output', action='store_true', help='本地模型只生成动作名')
    parser.add_argument('--local-process-record', type=Path,
                        default=ROOT/'Saved/AIResearch/LocalNpcModel/qwen3-1.7b/running.json')
    parser.add_argument('--request-timeout', type=float, default=10.)
    parser.add_argument('--warmup-timeout', type=float, help='冷预热的独立限时，0.1–30 秒；省略时与正式请求相同')
    parser.add_argument('--decision-interval-seconds', type=float, default=1., help='各组使用相同发起间隔，慢请求不补发')
    parser.add_argument('--seed', type=int, default=20261114)
    parser.add_argument('--runtime', default=DEFAULT_RUNTIME)
    parser.add_argument('--tts-config', type=Path)
    parser.add_argument('--chunk-frames', type=int, choices=(4, 8), default=8)
    parser.add_argument('--start-buffer', type=float, default=1.28)
    parser.add_argument('--duration-seconds', type=float, default=40.)
    parser.add_argument('--wait-seconds', type=float, default=15.)
    parser.add_argument('--speech-gap-seconds', type=float, default=.2)
    parser.add_argument('--cadence-report', type=Path, help='复用 baseline 的四句发起间隔，保持各组相同负载')
    parser.add_argument('--game-state', choices=('unknown', 'editor_idle', 'single_pie', 'dual_pie'), default='unknown')
    parser.add_argument('--run', action='store_true')
    args = parser.parse_args()
    if args.compact_output and args.provider != 'local':
        parser.error('--compact-output 只用于本地模型组')
    warmup_timeout = args.request_timeout if args.warmup_timeout is None else args.warmup_timeout
    if (not 40 <= args.duration_seconds <= 120 or not 0 <= args.wait_seconds <= 120
            or not .1 <= args.request_timeout <= 30 or not 0 <= args.speech_gap_seconds <= 2
            or not .01 <= args.start_buffer <= 3 or not .1 <= args.decision_interval_seconds <= 30):
        parser.error('时段需 40–120 秒，等待 0–120 秒，请求限时 0.1–30 秒，语音间隔 0–2 秒')
    if not math.isfinite(warmup_timeout) or not .1 <= warmup_timeout <= 30:
        parser.error('预热限时需为 0.1–30 秒')
    cases, source_hash = load_cases(args.source)
    print(json.dumps({'status': 'prepared', 'provider': args.provider, 'label': args.label, 'cases': len(cases),
                      'source_sha256': source_hash, 'run_requested': args.run,
                      'request_timeout_seconds': args.request_timeout, 'warmup_timeout_seconds': warmup_timeout}, ensure_ascii=False), flush=True)
    if not args.run:
        return
    output = args.output.resolve()
    output.relative_to((ROOT/'Saved/AIResearch').resolve())
    reject_existing_tts()
    output.mkdir(parents=True, exist_ok=False)
    engine, client, monitor = None, None, None
    report = {'schema_version': 1, 'usage': 'evaluation_only_not_training', 'status': 'preparing',
              'provider': args.provider, 'label': args.label, 'source_path': str(args.source.resolve()),
              'local_output_mode': ('action_name' if args.compact_output else 'json') if args.provider == 'local' else None,
              'source_sha256': source_hash, 'case_order': [case['id'] for case in cases],
              'labels_sent_to_model': False, 'instructions_sha256': content_hash(COMBAT_INSTRUCTIONS),
              'game_state_reported_by_operator': args.game_state, 'ue_before': process_snapshot(),
              'request_timeout_seconds': args.request_timeout, 'seed': args.seed, 'chunk_frames': args.chunk_frames,
              'warmup_timeout_seconds': warmup_timeout,
              'start_buffer_seconds': args.start_buffer, 'wait_seconds': args.wait_seconds,
              'decision_load': 'fixed_intervals_single_request_skip_missed_slots',
              'decision_interval_seconds': args.decision_interval_seconds,
              'sampling_local': {'temperature': 0., 'mode': 'greedy', 'top_p': .8, 'top_k': 20, 'min_p': 0},
              'evaluation_profile_routing': {
                  'note': '仅给评测副本补充正式人物编号，模型输入、标签和原始文件均不修改',
                  'name_to_profile': EVALUATION_PROFILES,
                  'added_case_ids': [case['id'] for case in cases
                                     if evaluation_context(case['context']).get('profile_id') != case['context'].get('profile_id')],
                  'allowed_profile_ids': os.environ.get('DELIVERY_COMBAT_MODEL_PROFILE_IDS', 'chunlai,xiumei')},
              'luna_sampling_note': '复用 AppServer 实际模型设置，不能假称与本地采样参数相同'}
    if args.local_process_record.is_file():
        process_record = json.loads(args.local_process_record.read_text(encoding='utf-8'))
        report['local_service_launch_record'] = {key: process_record.get(key) for key in
            ('pid', 'device', 'started_utc', 'gpu_layers', 'model_path', 'model_sha256', 'context_tokens', 'output_tokens', 'threads', 'threads_batch')}
        report['launch_record_process_identity_verified'] = False
    try:
        engine = NativeQwenSpeechSynthesizer(args.tts_config, args.runtime)
        report.update(tts_config_sha256=content_hash(engine.config), tts_runtime_sha256=content_hash(engine.runtime),
                      voice_identities=engine.identities, tts_config=engine.config)
        engine.prepare(threading.Event())
        report['tts_worker_pid'] = engine.process.pid if engine.process else None
        warmup = [synthesize_line(engine, line, threading.Event(), args.chunk_frames, args.start_buffer) for line in LINES]
        report['warmup_speech'] = warmup
        if any(row['status'] != 'completed' for row in warmup):
            raise RuntimeError('预热语音失败，未开始正式测量')
        cadence = [{'case': row['case'], 'text_sha256': content_hash(row['text']),
                    'audio_seconds': row['delivered_audio_seconds'],
                    'interval_seconds': row['delivered_audio_seconds']+args.speech_gap_seconds} for row in warmup]
        if args.cadence_report:
            previous = json.loads(args.cadence_report.read_text(encoding='utf-8'))
            for key in ('tts_config_sha256', 'tts_runtime_sha256', 'chunk_frames', 'source_sha256'):
                if previous[key] != report[key]:
                    raise ValueError('baseline 的语音配置不同：'+key)
            cadence = previous['speech_cadence']
            if (len(cadence) != len(LINES) or any(row['case'] != line[0] or row['text_sha256'] != content_hash(line[2])
                    or not math.isfinite(row['interval_seconds']) or row['interval_seconds'] <= 0
                    for row, line in zip(cadence, LINES))):
                raise ValueError('baseline 的语音时间表不符合当前四句台词')
            report['cadence_source'] = str(args.cadence_report.resolve())
        report['speech_cadence'] = cadence
        if args.provider == 'local':
            client = LocalCombatClient(args.endpoint, args.request_timeout, seed=args.seed, temperature=0.,
                                       compact_output=args.compact_output)
            if not client.is_idle():
                raise RuntimeError('本地服务未就绪或已有请求，未开始测量')
        elif args.provider == 'luna':
            client = LunaCombatClient(args.request_timeout)
        if client:
            warmup_model(client, report, warmup_timeout)
        report.update(status='ready', ready_utc=utc_now())
        (output/'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({'event': 'READY', 'utc': report['ready_utc'], 'wait_seconds': args.wait_seconds,
                          'duration_seconds': args.duration_seconds, 'output': str(output)}, ensure_ascii=False), flush=True)
        time.sleep(args.wait_seconds)
        monitor = GpuSamples()
        report['gpu_sampling_origin_utc'] = utc_now()
        report['gpu_sampling_includes_cancellation_cleanup'] = True
        report['measurement'] = run_window(engine, client, cases, cadence, args.duration_seconds,
                                            chunk_frames=args.chunk_frames, start_buffer=args.start_buffer,
                                            decision_interval=args.decision_interval_seconds)
        report['summary'] = summary(report['measurement'])
        report['status'] = 'completed' if not report['measurement']['errors'] else 'failed'
        if isinstance(client, LunaCombatClient):
            report['luna_settings'] = client.service.last_model_settings
    except BaseException as error:
        report.update(status='failed', error=type(error).__name__+':'+str(error))
        raise
    finally:
        if monitor:
            monitor.close()
            report['gpu_samples'] = monitor.samples
        for owned in (engine, client if isinstance(client, LunaCombatClient) else None):
            if owned:
                try:
                    owned.close()
                except Exception as error:
                    report.setdefault('cleanup_errors', []).append(str(error))
                    report['status'] = 'failed'
        report.update(ue_after=process_snapshot(), finished_utc=utc_now())
        (output/'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({'status': report['status'], 'output': str(output)}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
