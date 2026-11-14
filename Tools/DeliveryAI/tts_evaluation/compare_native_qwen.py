"""分别测现有 B 与原生 B，必须在已安排的 GPU 测试窗口运行"""
import argparse
import array
import datetime
import hashlib
import json
import os
from pathlib import Path
import sys
import subprocess
import threading
import time
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from neural_tts import QwenSpeechSynthesizer, NeuralTtsError, project_path
from native_tts import NativeQwenSpeechSynthesizer, DEFAULT_RUNTIME

LINES = [('male_short', 'male', '你先听我说完，我刚才真没看见你。'),
         ('female_short', 'female', '我就在旁边站着呢，你怎么能没看见呀？'),
         ('male_long', 'male', '刚才是我没看清楚。你拿好东西，我把车挪开，别挡着大家。'),
         ('female_long', 'female', '你别急着走呀。我刚才声音大了点，咱们好好说嘛。')]


def command_output(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=3,
                                encoding='mbcs' if os.name == 'nt' else 'utf-8', errors='replace',
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        return result.stdout.strip() if result.returncode == 0 else f'failed:{result.returncode}'
    except (OSError, subprocess.TimeoutExpired) as error:
        return type(error).__name__


def content_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False).encode('utf-8')).hexdigest()


def audio_onset(chunks, times, sample_rate):
    """记录实际音量起点，不把第一块回调直接当成玩家已经听见声音"""
    samples = array.array('h')
    samples.frombytes(b''.join(chunks))
    window = sample_rate // 50
    first = None
    for offset in range(0, len(samples) - window + 1, window):
        block = samples[offset:offset + window]
        rms = (sum(value * value for value in block) / window) ** .5 / 32768
        if rms >= .01:
            first = offset
            break
    delivery = None
    if first is not None:
        delivered_samples = 0
        for chunk, timestamp in zip(chunks, times):
            delivered_samples += len(chunk) // 2
            if delivered_samples >= first + window:
                delivery = timestamp
                break
    first_block = samples[:len(chunks[0]) // 2]
    return {'first_chunk_samples': len(first_block),
            'first_chunk_rms': round((sum(value * value for value in first_block) / len(first_block)) ** .5 / 32768, 6),
            'first_chunk_peak': max(abs(value) for value in first_block) / 32768,
            'audible_rms_threshold': .01, 'audible_window_ms': 20,
            'first_audible_audio_offset_ms': None if first is None else first / sample_rate * 1000,
            'first_audible_delivered_ms': delivery,
            'pcm_sha256': hashlib.sha256(samples.tobytes()).hexdigest()}


class GpuSamples:
    """只读取 GPU 状态，采样间隔一秒，原版与原生对照使用相同方式"""
    def __init__(self):
        self.samples, self.closed = [], threading.Event()
        self.started = time.perf_counter()
        self.thread = threading.Thread(target=self.run, name='native-compare-gpu', daemon=True)
        self.thread.start()

    def run(self):
        while not self.closed.is_set():
            self.samples.append({'elapsed_ms': round((time.perf_counter() - self.started) * 1000, 2),
                'columns': 'memory.used MiB, utilization.gpu %, power.draw W, clocks.sm MHz',
                'value': command_output(['nvidia-smi', '--query-gpu=memory.used,utilization.gpu,power.draw,clocks.sm',
                                         '--format=csv,noheader,nounits'])})
            self.closed.wait(1)

    def close(self):
        self.closed.set()
        self.thread.join(4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=('current', 'native'), required=True)
    parser.add_argument('--runtime', default=DEFAULT_RUNTIME)
    parser.add_argument('--output', required=True)
    parser.add_argument('--short-only', action='store_true')
    parser.add_argument('--cancel-check', action='store_true')
    parser.add_argument('--repeats', type=int, choices=(1, 2, 3), default=1)
    args = parser.parse_args()
    output = project_path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    engine = (QwenSpeechSynthesizer() if args.backend == 'current' else
              NativeQwenSpeechSynthesizer(runtime_path=args.runtime))
    config = engine.config
    sampling = {'seed': config['seed'], 'do_sample': True, 'temperature': .9, 'top_k': 50,
                'top_p': 1, 'repetition_penalty': 1.05, 'subtalker_temperature': .9,
                'subtalker_top_k': 50, 'subtalker_top_p': 1, 'max_new_tokens': config['max_new_tokens']}
    results = {'backend': args.backend, 'runtime': args.runtime, 'status': 'running', 'utterances': [],
        'utc_started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'clock': vars(time.get_clock_info('perf_counter')),
        'config': config, 'config_sha256': content_hash(config), 'sampling': sampling,
        'sampling_sha256': content_hash(sampling), 'voice_identities': engine.identities,
        'hardware': command_output(['nvidia-smi', '--query-gpu=name,driver_version,memory.total', '--format=csv']),
        'ue_processes_before': command_output(['tasklist', '/FI', 'IMAGENAME eq UnrealEditor.exe', '/FO', 'CSV', '/NH'])}
    if args.backend == 'native':
        results['runtime_sha256'] = content_hash(engine.runtime)
    monitor = GpuSamples()
    def save():
        (output / 'summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    try:
        start = time.perf_counter()
        engine.prepare(threading.Event())
        results['prepare_ms'] = round((time.perf_counter() - start) * 1000, 2)
        selected = LINES[:2] if args.short_only else LINES
        cases = [(f'{name}_round{repeat + 1}', voice, text) for repeat in range(args.repeats)
                 for name, voice, text in selected]
        for name, voice, text in cases:
            chunks, times = [], []
            start = time.perf_counter()
            def receive(pcm):
                times.append(round((time.perf_counter() - start) * 1000, 2))
                chunks.append(pcm)
            total, sample_rate = engine.synthesize_stream(text, voice, 'Calm', threading.Event(), receive)
            elapsed = time.perf_counter() - start
            path = output / f'{name}.wav'
            with wave.open(str(path), 'wb') as audio:
                audio.setparams((1, 2, sample_rate, 0, 'NONE', 'not compressed'))
                audio.writeframes(b''.join(chunks))
            duration = total / (2 * sample_rate)
            results['utterances'].append(dict(name=name, voice=voice, text=text, first_audio_ms=times[0],
                elapsed_ms=round(elapsed * 1000, 2), audio_seconds=duration,
                generation_rtf=round(elapsed / duration, 4), chunk_times_ms=times,
                chunk_bytes=[len(chunk) for chunk in chunks],
                chunk_duration_ms=[len(chunk) / (2 * sample_rate) * 1000 for chunk in chunks],
                audio_onset=audio_onset(chunks, times, sample_rate), wav=str(path)))
            save()
        if args.cancel_check:
            cancel = threading.Event()
            start = time.perf_counter()
            count = 0
            cancel_requested_ms = None
            def receive_cancel(_):
                nonlocal count, cancel_requested_ms
                count += 1
                cancel_requested_ms = (time.perf_counter() - start) * 1000
                cancel.set()
            try:
                engine.synthesize_stream(LINES[2][2], 'male', 'Calm', cancel, receive_cancel)
                raise AssertionError('cancel_was_ignored')
            except NeuralTtsError as error:
                if error.code != 'cancelled':
                    raise
            cancelled_ms = round((time.perf_counter() - start) * 1000, 2)
            engine.synthesize_stream(LINES[1][2], 'female', 'Calm', threading.Event(), lambda _: None)
            results['cancel_check'] = {'delivered_chunks': count, 'next_female_completed': True,
                                      'cancel_requested_ms': round(cancel_requested_ms, 2),
                                      'cancel_confirmed_ms': cancelled_ms,
                                      'cancel_latency_ms': round(cancelled_ms - cancel_requested_ms, 2),
                                      'elapsed_with_next_ms': round((time.perf_counter() - start) * 1000, 2)}
        results['status'] = 'completed'
    except Exception as error:
        results.update(status='failed', error=repr(error))
        raise
    finally:
        try:
            engine.close()
        finally:
            monitor.close()
            results['gpu_samples'] = monitor.samples
            results['ue_processes_after'] = command_output(['tasklist', '/FI', 'IMAGENAME eq UnrealEditor.exe', '/FO', 'CSV', '/NH'])
            save()


if __name__ == '__main__':
    main()
