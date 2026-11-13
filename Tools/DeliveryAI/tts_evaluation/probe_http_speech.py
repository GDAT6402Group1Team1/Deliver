"""使用正式 HTTP、语音队列与合成接口比较整句和分段；默认 CPU 替身，--gpu 才加载 Qwen"""
import argparse
import base64
from contextlib import ExitStack
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import urllib.request
import uuid
from unittest.mock import patch
import wave

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'Tools/DeliveryAI'))
import streaming
from neural_tts import CONFIG_PATH, QwenSpeechSynthesizer, load_voice_config, neural_voice_identities, project_path
from server import DecisionServer, validate_action
from tts_evaluation.evaluate import GpuSampler

CASES = (
    ('male_short_1', 'male', '翠花，你别急，先听我把话说完。'),
    ('female_short_1', 'female', '哎，你找我什么事？'),
    ('male_short_2', 'male', '谁说我怕他，你叫他过来。'),
    ('female_short_2', 'female', '你别听他瞎说，我刚才说的是车。'),
    ('male_long', 'male', '你先把车停到旁边，别听他乱传，我刚才说的是车挡住路了。'),
    ('female_long', 'female', '车停在门口，菜篮子都过不去，我让他挪一下，他倒先嚷起来了。'),
)
MODES = ('whole', '4', '8')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def save_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')


def buffering_metrics(chunks, end_ms, start_buffer=.64, resume_buffer=.32):
    """按实际收包时间回看声音是否够用，不包含 UE 编解码、网络复制和声卡延迟"""
    if not chunks:
        return None
    previous = 0.
    lower_bound = 0.
    start = None
    initial_audio = 0.
    for part in chunks:
        when = part['received_ms'] / 1000
        lower_bound = max(lower_bound, when - previous)
        previous += part['duration_s']
        if start is None and previous + 1e-9 >= start_buffer:
            start, initial_audio = when, previous
    duration = previous
    if start is None:
        start, initial_audio = end_ms / 1000, duration
    previous = 0.
    margins = [initial_audio]
    for part in chunks:
        when = part['received_ms'] / 1000
        if when > start + 1e-9:
            margins.append(start + previous - when)
        previous += part['duration_s']

    # 缺音后积累续播缓冲再继续；短尾音在句末到达后允许继续播放
    buffer = received = played = 0.
    last = 0.
    playing = False
    started = False
    gap_start = None
    stalls = []
    events = [(part['received_ms']/1000, part['duration_s'], False) for part in chunks]
    events.append((end_ms/1000, 0., True))
    for when, added, final in events:
        if playing:
            elapsed = max(0., when-last)
            consumed = min(buffer, elapsed)
            buffer -= consumed
            played += consumed
            if elapsed > consumed + 1e-9 and played < duration - 1e-9:
                gap_start = last+consumed
                playing = False
        buffer += added
        received += added
        threshold = resume_buffer if started else start_buffer
        if not playing and buffer > 0 and (buffer+1e-9 >= threshold or final):
            if gap_start is not None:
                stalls.append({'start_ms': gap_start*1000, 'end_ms': when*1000,
                               'duration_ms': max(0., when-gap_start)*1000})
                gap_start = None
            playing = started = True
        last = when
    minimum = min(margins)
    return {
        'duration_s': duration,
        'retrospective_uninterrupted_start_ms': lower_bound*1000,
        'buffer_064_start_ms': start*1000,
        'buffer_064_min_margin_ms': minimum*1000,
        'buffer_064_max_missing_ms': max(0., -minimum)*1000,
        'buffer_064_extra_delay_needed_ms': max(0., lower_bound-start)*1000,
        'simulated_resume_buffer_s': resume_buffer,
        'simulated_stalls': stalls,
        'simulated_total_stall_ms': sum(item['duration_ms'] for item in stalls),
        'simulated_max_stall_ms': max((item['duration_ms'] for item in stalls), default=0.),
        'simulation_only_not_ue_playback': True,
    }


class FakeSynth:
    """只用于检查探针和 HTTP 是否工作，不用于报告真实声音性能"""
    supports_audio_stream = True
    voice_name = 'CPU_FAKE'
    process = None

    def prepare(self, cancel):
        return True

    def synthesize(self, text, voice, emotion, cancel):
        if cancel.wait(.018):
            raise RuntimeError('cancelled')
        return b'\1\0'*23040, 24000

    def synthesize_stream(self, text, voice, emotion, cancel, on_chunk, chunk_frames=8):
        pcm = b'\1\0'*23040
        size = chunk_frames*3840
        for offset in range(0, len(pcm), size):
            if cancel.wait(.006):
                raise RuntimeError('cancelled')
            on_chunk(pcm[offset:offset+size])
        return len(pcm), 24000

    def close(self):
        pass


class TraceSynth:
    """只记录正式接口的返回时间；4/8 帧选择在本探针中传给原接口"""
    def __init__(self, inner):
        self.inner = inner
        self.row = None
        self.origin = 0.
        self.mode = 'whole'

    @property
    def supports_audio_stream(self):
        return self.inner.supports_audio_stream

    @property
    def voice_name(self):
        return self.inner.voice_name

    def elapsed(self):
        return (time.perf_counter()-self.origin)*1000

    def prepare(self, cancel):
        return self.inner.prepare(cancel)

    def record_pcm(self, pcm):
        now = self.elapsed()
        self.row.setdefault('server_first_pcm_ms', now)
        self.row['server_pcm_chunks'].append({'ready_ms': now, 'bytes': len(pcm), 'duration_s': len(pcm)/48000})

    def synthesize(self, text, voice, emotion, cancel):
        self.row['tts_enter_ms'] = self.elapsed()
        pcm, rate = self.inner.synthesize(text, voice, emotion, cancel)
        self.row['voice_name'] = self.inner.voice_name
        self.record_pcm(pcm)
        self.row['tts_return_ms'] = self.elapsed()
        return pcm, rate

    def synthesize_stream(self, text, voice, emotion, cancel, on_chunk, chunk_frames=8):
        self.row['tts_enter_ms'] = self.elapsed()
        def forward(pcm):
            self.record_pcm(pcm)
            on_chunk(pcm)
        result = self.inner.synthesize_stream(text, voice, emotion, cancel, forward, chunk_frames=int(self.mode))
        self.row['voice_name'] = self.inner.voice_name
        self.row['tts_return_ms'] = self.elapsed()
        return result


def context(voice, mode):
    return {'protocol_version': 1, 'capability_version': 1,
            **{key: str(uuid.uuid4()) for key in ('world_epoch', 'npc_id', 'request_id')},
            'context_revision': 0, 'plan_revision': 1, 'persona': '语音性能试验，台词已经固定',
            'request_source': 'player', 'voice': voice, 'allowed_actions': ['Wait'], 'evidence': [],
            'candidates': [{'action': 'Wait', 'target_id': '', 'duration': 1, 'evidence_ids': []}],
            'speech_transport': 'sentence' if mode == 'whole' else 'pcm_chunks_v1'}


def request_sample(server, traced, output, name, voice, text, mode):
    row = {'case': name, 'voice': voice, 'voice_name': 'unknown', 'text': text, 'mode': mode, 'status': 'started',
           'server_pcm_chunks': [], 'http_chunks': [], 'http_events': []}
    traced.row, traced.mode = row, mode
    data = context(voice, mode)
    row['request_id'] = data['request_id']
    request = urllib.request.Request(f'http://127.0.0.1:{server.server_port}/v1/decisions/stream',
                                     json.dumps(data, ensure_ascii=False).encode(), {'Content-Type': 'application/json'})
    pcm = bytearray()
    begun = terminal = done = False
    rate = 24000
    def fixed_reply(context, provider):
        row['fixed_model_reply_ms'] = traced.elapsed()
        return validate_action(context, {**context['candidates'][0], 'speech': text, 'emotion': 'Calm'})
    try:
        with patch('server.decide', side_effect=fixed_reply):
            traced.origin = time.perf_counter()
            row['request_started_utc'] = datetime.now(timezone.utc).isoformat()
            with urllib.request.urlopen(request, timeout=10) as response:
                if response.status != 200:
                    raise RuntimeError('unexpected_http_status')
                wire_bytes = 0
                for line in response:
                    now = traced.elapsed()
                    wire_bytes += len(line)
                    if wire_bytes > 1024*1024 or now > 120000:
                        raise RuntimeError('probe_response_limit')
                    event = json.loads(line)
                    kind = event['type']
                    if kind == 'keepalive':
                        continue
                    if event.get('request_id') != data['request_id']:
                        raise RuntimeError('wrong_request_id')
                    row['http_events'].append({'type': kind, 'received_ms': now})
                    if kind == 'error':
                        raise RuntimeError('backend_error:'+str(event.get('reason', 'unknown')))
                    if kind == 'speech_begin':
                        if begun or event.get('sequence') != 0 or event.get('text') != text or event.get('sample_rate') != 24000:
                            raise RuntimeError('invalid_begin')
                        begun = True
                    if kind in ('sentence', 'audio_chunk'):
                        if event.get('sequence') != 0 or terminal:
                            raise RuntimeError('invalid_audio_sequence')
                        if kind == 'audio_chunk' and (not begun or event['chunk_index'] != len(row['http_chunks']) or event['offset_bytes'] != len(pcm)):
                            raise RuntimeError('invalid_chunk_order')
                        if kind == 'sentence':
                            if mode != 'whole' or pcm or event.get('text') != text:
                                raise RuntimeError('unexpected_whole_sentence')
                            rate = event['sample_rate']
                        block = base64.b64decode(event['pcm'], validate=True)
                        if len(block)%2 or len(pcm)+len(block)>384000 or rate!=24000:
                            raise RuntimeError('invalid_pcm')
                        if block:
                            row.setdefault('first_audio_http_ms', now)
                            row['http_chunks'].append({'index': len(row['http_chunks']), 'received_ms': now,
                                                       'offset_bytes': len(pcm), 'bytes': len(block), 'duration_s': len(block)/(2*rate)})
                            pcm.extend(block)
                        if kind == 'sentence':
                            terminal = True
                            row['audio_end_http_ms'] = now
                            row['audio_status'] = 'completed' if block else 'failed'
                            if not block:
                                row['audio_error'] = event.get('audio_error', 'empty_audio')
                    if kind == 'audio_end':
                        if not begun or terminal or event.get('sequence') != 0 or event['chunk_count'] != len(row['http_chunks']) or event['total_pcm_bytes'] != len(pcm):
                            raise RuntimeError('invalid_end')
                        terminal = True
                        row['audio_end_http_ms'] = now
                        row['audio_status'] = event['status']
                        if event['status'] != 'completed':
                            row['audio_error'] = event.get('audio_error', 'unknown')
                    if kind == 'done':
                        if not terminal or done or event.get('sentences') != 1:
                            raise RuntimeError('invalid_done')
                        done = True
                        row['done_http_ms'] = now
            if not terminal or not done:
                raise RuntimeError('missing_terminal')
        row['status'] = 'completed' if row['audio_status'] == 'completed' else 'tts_failed'
        if row['status'] == 'completed':
            row['buffering'] = buffering_metrics(row['http_chunks'], row['audio_end_http_ms'])
    except Exception as error:
        row.update(status='failed', error=str(error), error_type=type(error).__name__)
        # 仅取消这个临时服务的本次任务，确认结束后才切换下一条固定台词
        server.scheduler.cancel(tuple(data[key] for key in ('world_epoch', 'npc_id', 'request_id')))
    finally:
        deadline = time.monotonic()+8
        key = tuple(data[key] for key in ('world_epoch', 'npc_id', 'request_id'))
        while time.monotonic() < deadline:
            with server.scheduler.condition:
                if key not in server.scheduler.jobs:
                    break
            time.sleep(.01)
        else:
            raise RuntimeError('probe_job_did_not_finish')
    row['pcm_bytes'] = len(pcm)
    row['pcm_sha256'] = sha(pcm)
    row['duration_s'] = len(pcm)/(2*rate)
    if pcm:
        suffix = '' if row['status']=='completed' else '_partial'
        path = output / f'{name}_{mode}{suffix}.wav'
        with wave.open(str(path), 'wb') as audio:
            audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(rate); audio.writeframes(pcm)
        row['wav'] = str(path.relative_to(ROOT))
    row['worker_pid'] = getattr(getattr(traced.inner, 'process', None), 'pid', None)
    return row


def check_gpu_window():
    if os.name != 'nt':
        raise RuntimeError('GPU 试验的进程检查目前只支持本项目的 Windows 环境')
    command = "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -or ($_.Name -match '^python(w)?\\.exe$' -and $_.CommandLine -match 'neural_tts_worker\\.py') } | Select-Object -ExpandProperty ProcessId"
    result = subprocess.run(['powershell', '-NoProfile', '-Command', command], capture_output=True, text=True,
                            check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.stdout.strip():
        raise RuntimeError('请由主任务先确认关闭编辑器和正式语音工作进程，再运行独立 GPU 对照')


def run(output, gpu=False, config=None, cases=CASES):
    if gpu:
        check_gpu_window()
    config_path = project_path(config or CONFIG_PATH)
    settings = load_voice_config(config_path)
    if settings['seed'] != 20261113:
        raise ValueError('probe_requires_production_seed')
    output = project_path(output)
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'started', 'measurement': 'real_qwen_http' if gpu else 'cpu_fake_http_precheck',
              'fixed_model_reply': True, 'llm_invoked': False, 'gpu_requested': gpu,
              'game_running_during_probe': False if gpu else None, 'ue_playback_measured': False,
              'seed': settings['seed'], 'voice_identities': neural_voice_identities(settings),
              'config_sha256_before': sha(config_path.read_bytes()), 'samples': [],
              'limits': ['每个台词和模式只运行一次，没有 P95 或全面稳定性结论',
                         '模型台词固定，只测语音等待，不代表玩家操作到首声',
                         '本机 HTTP 到达时间不包含 UE 编解码、网络复制与实际声卡播放',
                         '缓冲余量和 0.32 秒续播是模拟，最早连续起播时间是事后下界',
                         '保持正式 seed，不测种子变化；8 帧 WAV 仅供候选试听']}
    manifest = output/'Manifest.json'
    save_json(manifest, report)
    synth = QwenSpeechSynthesizer(config_path) if gpu else FakeSynth()
    traced = TraceSynth(synth)
    sampler = GpuSampler() if gpu else None
    server = thread = None
    try:
        if sampler:
            sampler.start()
        started = time.perf_counter()
        synth.prepare(threading.Event())
        report['voice_prepare_ms'] = (time.perf_counter()-started)*1000
        report['worker_pid'] = getattr(getattr(synth, 'process', None), 'pid', None)
        print(f"probe voice_prepared elapsed_ms={report['voice_prepare_ms']:.3f} worker_pid={report['worker_pid']}", flush=True)
        with ExitStack() as stack:
            stack.enter_context(patch.dict(os.environ, {'DELIVERY_AI_STREAM_AUDIO': '1'}))
            stack.enter_context(patch('streaming.uses_neural_speech', return_value=True))
            stack.enter_context(patch('streaming.neural_speech_synthesizer', return_value=traced))
            server = DecisionServer(('127.0.0.1', 0), 'fixture')
            report['temporary_http_port'] = server.server_port
            thread = threading.Thread(target=server.serve_forever, name='speech-http-probe')
            thread.start()
            for index, (name, voice, text) in enumerate(cases):
                if streaming.split_speech(text) != [text]:
                    raise ValueError('case_must_be_one_complete_sentence')
                modes = MODES[index%3:]+MODES[:index%3]
                for mode in modes:
                    row = request_sample(server, traced, output, name, voice, text, mode)
                    report['samples'].append(row)
                    save_json(manifest, report)
                    print(json.dumps({key: row.get(key) for key in ('case', 'mode', 'status', 'first_audio_http_ms', 'audio_end_http_ms')}, ensure_ascii=False), flush=True)
            for row in report['samples']:
                baseline = next(item for item in report['samples'] if item['case']==row['case'] and item['mode']=='whole')
                if row['status']=='completed' and baseline['status']=='completed':
                    row['matches_whole_pcm'] = row['pcm_sha256']==baseline['pcm_sha256']
                    row['matches_whole_length'] = row['pcm_bytes']==baseline['pcm_bytes']
            report['status'] = 'completed' if all(row['status']=='completed' for row in report['samples']) else 'completed_with_sample_failures'
    except BaseException as error:
        report.update(status='failed', error_type=type(error).__name__, error=str(error))
        raise
    finally:
        try:
            if server:
                if thread and thread.is_alive():
                    server.shutdown()
                server.server_close()
        finally:
            if thread:
                thread.join(3)
            try:
                synth.close()
            finally:
                if sampler:
                    report['gpu_sampling'] = sampler.finish()
                    utilization = {}
                    for sample in report['gpu_sampling']['samples']:
                        for device in sample['devices']:
                            key = str(device['index'])
                            utilization[key] = max(utilization.get(key, 0), device['utilization_percent'])
                    report['gpu_sampling']['sampled_peak_utilization_percent_by_gpu'] = utilization
                report['worker_closed'] = getattr(synth, 'process', None) is None
                report['http_thread_closed'] = thread is None or not thread.is_alive()
                report['config_sha256_after'] = sha(config_path.read_bytes())
                report['config_unchanged'] = report['config_sha256_after']==report['config_sha256_before']
                save_json(manifest, report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', action='store_true', help='明确加载正式 Qwen 模型；运行前须由主任务安排维护窗口')
    parser.add_argument('--config', help='项目内候选配置路径，不修改正式配置')
    parser.add_argument('--output', help='项目内尚不存在的结果目录')
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = args.output or f'Saved/AIResearch/HttpSpeechProbe/{stamp}'
    report = run(output, args.gpu, args.config)
    print(str(project_path(output)/'Manifest.json'), flush=True)
    return 0 if report['status']=='completed' else 1


if __name__=='__main__':
    raise SystemExit(main())
