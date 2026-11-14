"""在同一份 Qwen 模型上比较语音实现，保存逐块耗时与录音；加 --gpu 才运行合成"""
import argparse
import base64
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from types import SimpleNamespace
import uuid
import wave


ROOT = Path(__file__).resolve().parents[3]
AI_ROOT = ROOT / 'Tools/DeliveryAI'
sys.path.insert(0, str(AI_ROOT))
from neural_tts import load_voice_config, neural_voice_identities
from tts_evaluation.probe_http_speech import CASES, buffering_metrics


def digest(data):
    return hashlib.sha256(data).hexdigest()


def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def source_folder(path):
    folder = Path(path).resolve()
    if (folder / 'Tools/DeliveryAI/neural_tts_worker.py').is_file():
        folder /= 'Tools/DeliveryAI'
    for name in ('neural_audio_stream.py', 'neural_tts_worker.py'):
        if not (folder / name).is_file():
            raise ValueError(f'源码目录缺少 {name}: {folder}')
    return folder


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    return module


def load_variant(folder, label):
    """旧版只提供生成方法，模型、配置和声音资源都使用当前工程的同一份"""
    previous = sys.modules.get('neural_audio_stream')
    sys.path.insert(0, str(folder))
    try:
        stream = load_module('_speech_probe_' + label + '_stream', folder / 'neural_audio_stream.py')
        sys.modules['neural_audio_stream'] = stream
        return load_module('_speech_probe_' + label + '_worker', folder / 'neural_tts_worker.py')
    finally:
        sys.path.remove(str(folder))
        if previous is None:
            sys.modules.pop('neural_audio_stream', None)
        else:
            sys.modules['neural_audio_stream'] = previous


def existing_workers():
    if os.name != 'nt':
        return []
    result = subprocess.run(['powershell', '-NoProfile', '-Command',
        "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'python*' } | "
        "Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress"],
        capture_output=True, text=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    rows = json.loads(result.stdout) if result.stdout.strip() else []
    if isinstance(rows, dict):
        rows = [rows]
    return [row['ProcessId'] for row in rows
            if row['ProcessId'] != os.getpid() and 'neural_tts_worker.py' in (row.get('CommandLine') or '')]


class AudioTrace:
    """记录合成接口真正交出声音的时间，缺块或重复块直接让本次检查失败"""
    def __init__(self, origin, limit=384000):
        self.origin, self.limit = origin, limit
        self.parts, self.chunks = [], []
        self.size = 0

    def receive(self, message):
        received_ms = (time.perf_counter() - self.origin) * 1000
        pcm = base64.b64decode(message['pcm'], validate=True)
        if (message['type'] != 'audio_chunk' or message['chunk_index'] != len(self.parts)
                or message['offset_bytes'] != self.size or message['sample_rate'] != 24000
                or not pcm or len(pcm) % 2 or self.size + len(pcm) > self.limit):
            raise ValueError('语音块重复、缺失或长度不符')
        self.parts.append(pcm)
        self.chunks.append({'received_ms': received_ms, 'duration_s': len(pcm) / 48000,
                            'bytes': len(pcm), 'offset_bytes': self.size})
        self.size += len(pcm)

    def finish(self, result, elapsed_ms, start_buffer=1.28):
        if result['type'] == 'audio':
            if self.parts or result['sample_rate'] != 24000:
                raise ValueError('完整语音与分段语音混用')
            pcm = base64.b64decode(result['pcm'], validate=True)
            if not pcm or len(pcm) % 2 or len(pcm) > self.limit:
                raise ValueError('完整语音长度不符')
            self.parts, self.size = [pcm], len(pcm)
            self.chunks = [{'received_ms': elapsed_ms, 'duration_s': len(pcm) / 48000,
                            'bytes': len(pcm), 'offset_bytes': 0}]
        elif (result['type'] != 'audio_end' or result['total_pcm_bytes'] != self.size
              or result['chunk_count'] != len(self.parts) or not self.parts):
            raise ValueError('语音结束长度不符')
        pcm = b''.join(self.parts)
        simulation = buffering_metrics(self.chunks, elapsed_ms, start_buffer=start_buffer)
        # 旧探针键名包含 064，统一改为无参数名称，另存本次实际缓冲设置
        simulation = {key.replace('buffer_064_', 'buffer_'): value for key, value in simulation.items()}
        simulation['start_buffer_s'] = start_buffer
        return pcm, {'first_chunk_ms': self.chunks[0]['received_ms'], 'complete_ms': elapsed_ms,
                     'audio_duration_s': len(pcm) / 48000,
                     'generation_rtf': elapsed_ms / 1000 / (len(pcm) / 48000),
                     'chunks': self.chunks, 'buffer_simulation': simulation}


@contextmanager
def trace_model(engine, enabled=True):
    """合成后再读取 GPU 计时，逐块检查时不额外同步显卡"""
    torch = engine.torch
    codes, decode_events, pending = [], [], []
    def observe_codes(_module, _args, _kwargs, output):
        hidden = output.hidden_states
        if isinstance(hidden, tuple) and len(hidden) == 2 and hidden[1] is not None:
            codes.append(hidden[1].detach().clone())
    def begin_decode(_module, args, kwargs):
        value = args[0] if args else kwargs['codes']
        started = torch.cuda.Event(enable_timing=True)
        ended = torch.cuda.Event(enable_timing=True)
        started.record()
        pending.append((started, ended, time.perf_counter(), int(value.shape[-1])))
    def end_decode(_module, _args, _kwargs, _output):
        started, ended, wall, frames = pending.pop()
        ended.record()
        decode_events.append((started, ended, (time.perf_counter() - wall) * 1000, frames))
    handles = [engine.model.model.talker.register_forward_hook(observe_codes, with_kwargs=True)]
    if enabled:
        decoder = engine.model.model.speech_tokenizer.model.decoder
        handles.extend((decoder.register_forward_pre_hook(begin_decode, with_kwargs=True),
                        decoder.register_forward_hook(end_decode, with_kwargs=True)))
    record = {}
    try:
        yield record
    finally:
        for handle in handles:
            handle.remove()
        engine.synchronize()
        record['codec_sha256'] = digest(torch.cat(codes).cpu().numpy().tobytes()) if codes else None
        record['codec_frames_including_eos'] = len(codes)
        record['decoder_calls'] = [{'frames': frames, 'cpu_dispatch_ms': wall,
                                    'gpu_elapsed_ms': started.elapsed_time(ended)}
                                   for started, ended, wall, frames in decode_events]
        record['decoder_gpu_total_ms'] = sum(row['gpu_elapsed_ms'] for row in record['decoder_calls'])
        record['decoder_profile_enabled'] = enabled


def run_case(worker, engine, case, mode, output, profile=True):
    name, voice, text = case
    task = SimpleNamespace(task_id=uuid.uuid4().hex, voice=voice, text=text, emotion='Calm',
                           speaker=engine.config['voices'][voice]['speaker'],
                           chunk_frames=0 if mode == 'whole' else int(mode), cancel=threading.Event())
    method = worker.QwenEngine.synthesize if voice == 'male' else worker.QwenCloneEngine.synthesize
    closed = threading.Event()
    with trace_model(engine, profile) as model_trace:
        origin = time.perf_counter()
        audio = AudioTrace(origin)
        result = method(engine, task, closed, audio.receive if task.chunk_frames else None)
        elapsed_ms = (time.perf_counter() - origin) * 1000
    pcm, row = audio.finish(result, elapsed_ms)
    path = output / f'{name}-{mode}.wav'
    with wave.open(str(path), 'wb') as sound:
        sound.setnchannels(1)
        sound.setsampwidth(2)
        sound.setframerate(24000)
        sound.writeframes(pcm)
    row.update(case=name, voice=voice, text=text, mode=mode, wav=str(path),
               pcm_sha256=digest(pcm), **model_trace)
    return row


def compare_rows(rows):
    pairs = []
    for current in rows:
        if current['variant'] != 'current':
            continue
        before = next((row for row in rows if row['variant'] == 'baseline'
                       and row['case'] == current['case'] and row['mode'] == current['mode']), None)
        if before:
            pairs.append({'case': current['case'], 'mode': current['mode'],
                          'same_codec': bool(current['codec_sha256']) and before['codec_sha256'] == current['codec_sha256'],
                          'same_pcm': before['pcm_sha256'] == current['pcm_sha256'],
                          'first_chunk_change_ms': current['first_chunk_ms'] - before['first_chunk_ms'],
                          'complete_change_ms': current['complete_ms'] - before['complete_ms']})
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', action='store_true', help='加载模型并合成；默认只检查路径和配置')
    parser.add_argument('--source-root', type=Path, help='旧源码目录，含 neural_tts_worker.py 和 neural_audio_stream.py')
    parser.add_argument('--case', choices=['all', *(case[0] for case in CASES)], default='all')
    parser.add_argument('--mode', choices=['whole', '4', '8'], default='8')
    parser.add_argument('--variant', choices=['both', 'current', 'baseline'], default='both')
    parser.add_argument('--game-state', choices=['unknown', 'closed', 'editor_idle', 'game_running'], default='unknown')
    parser.add_argument('--no-decoder-profile', action='store_true', help='关闭解码 GPU 计时，复查探针对总耗时的影响')
    parser.add_argument('--output', type=Path, default=ROOT / 'Saved/AIResearch/StreamRuntime' /
                        datetime.now().strftime('%Y%m%d-%H%M%S'))
    args = parser.parse_args()
    if args.variant == 'baseline' and not args.source_root:
        parser.error('--variant baseline 需要 --source-root')
    folders = {'current': AI_ROOT}
    if args.source_root:
        folders['baseline'] = source_folder(args.source_root)
    if args.variant != 'both':
        folders = {args.variant: folders[args.variant]}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config = load_voice_config()
    report = {'created_utc': datetime.now(timezone.utc).isoformat(), 'gpu_loaded': False,
              'game_state_reported_by_operator': args.game_state, 'ue_playback_measured': False,
              'transport': 'direct_worker_callback_not_http', 'same_model_instance': True,
              'voice_identities': neural_voice_identities(config), 'rows': [],
              'source_sha256': {label: {name: digest((folder / name).read_bytes())
                  for name in ('neural_tts_worker.py', 'neural_audio_stream.py')}
                  for label, folder in folders.items()}}
    save_json(output / 'summary.json', report)
    if not args.gpu:
        print(json.dumps({'preflight': 'passed', 'gpu_loaded': False, 'summary': str(output / 'summary.json')}))
        return
    if workers := existing_workers():
        raise RuntimeError(f'现有语音进程仍在运行，停止独立试验，避免重复占用显存: {workers}')
    import neural_tts_worker as current
    variants = {'current': current}
    if 'baseline' in folders:
        variants['baseline'] = load_variant(folders['baseline'], 'baseline')
    closed = threading.Event()
    runtime = current.QwenVoiceEngines(config, closed)
    report['gpu_loaded'] = True
    cases = [case for case in CASES if args.case == 'all' or case[0] == args.case]
    try:
        # 每种声音先用同一句预热，冷启动不计入生成速度
        warmup = output / 'warmup'
        warmup.mkdir()
        for voice in dict.fromkeys(case[1] for case in cases):
            case = next(case for case in cases if case[1] == voice)
            run_case(current, runtime.engines[voice], case, args.mode, warmup, profile=False)
        for case in cases:
            for label in ('baseline', 'current'):
                if label not in folders:
                    continue
                folder = output / label
                folder.mkdir(exist_ok=True)
                row = run_case(variants[label], runtime.engines[case[1]], case, args.mode,
                               folder, profile=not args.no_decoder_profile)
                row['variant'] = label
                report['rows'].append(row)
                report['comparisons'] = compare_rows(report['rows'])
                save_json(output / 'summary.json', report)
                print(json.dumps({key: row[key] for key in ('variant', 'case', 'first_chunk_ms',
                                                          'complete_ms', 'generation_rtf')}, ensure_ascii=False), flush=True)
    except BaseException as error:
        report['error'] = {'type': type(error).__name__, 'message': str(error)}
        save_json(output / 'summary.json', report)
        raise
    finally:
        closed.set()
    print(json.dumps({'summary': str(output / 'summary.json')}, ensure_ascii=False))


if __name__ == '__main__':
    main()
