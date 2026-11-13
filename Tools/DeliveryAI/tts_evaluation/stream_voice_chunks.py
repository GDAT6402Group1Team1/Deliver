"""独立比较完整 B 语音与 4/8 帧分段解码；默认只做 CPU 检查，--gpu 才加载模型"""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
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
sys.path.insert(0, str(ROOT / 'Tools/DeliveryAI'))
from neural_tts import NeuralTtsError, load_voice_config, neural_voice_identities, project_path

SOURCES = [
    {'url': 'https://github.com/QwenLM/Qwen3-TTS/blob/main/qwen_tts/inference/qwen3_tts_model.py',
     'license': 'Apache-2.0', 'use': '沿用本机 0.1.1 完整生成和 Base 参考声音裁除方式'},
    {'url': 'https://arxiv.org/html/2601.15621v1#S3.SS4',
     'use': '12.5 Hz 每帧对应 80 毫秒声音；论文首包耗时不作为本机结果'},
    {'url': 'https://github.com/andimarafioti/faster-qwen3-tts/blob/a70afc0f81f7f5f8801c3227968f1102f43f211c/faster_qwen3_tts/model.py',
     'license': 'MIT', 'use': '参考先解码累计前缀、再取新增声音；本试验保留全部左上下文，不替换采样或启用 Talker Graph'},
]
CASES = {
    'short': '你别听他瞎说，我刚才说的是车。',
    'long': '你先把车停好。我刚才说的是车挡住路了，可没说你送得慢。',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def save_wav(path, pcm, rate=24000):
    with wave.open(str(path), 'wb') as sound:
        sound.setnchannels(1)
        sound.setsampwidth(2)
        sound.setframerate(rate)
        sound.writeframes(pcm)
    return {'wav': str(path), 'wav_sha256': sha(path.read_bytes()), 'pcm_sha256': sha(pcm),
            'frames': len(pcm) // 2, 'duration_s': len(pcm) / (2 * rate)}


def new_audio_span(samples, reference_frames, total_codec_frames, emitted_samples):
    # 与官方 Base 一致，先去掉参考声音，再取本轮尚未返回的部分
    if not 0 <= reference_frames < total_codec_frames or min(samples, emitted_samples) < 0:
        raise ValueError('invalid_decode_lengths')
    reference_samples = int(reference_frames / total_codec_frames * samples)
    generated_samples = samples - reference_samples
    if generated_samples < emitted_samples:
        raise ValueError('decoded_audio_moved_backwards')
    return reference_samples + emitted_samples, samples, reference_samples


class PcmChunks:
    """只记录这次试验的一句话，重复块、越界块和终止后的块直接拒绝"""
    def __init__(self, limit=384000):
        self.limit = limit
        self.parts = []
        self.size = 0
        self.terminal = None

    def append(self, sequence, offset, pcm, cancel=None):
        # 解码期间可能收到取消，真正追加声音前再检查一次
        if cancel is not None and cancel.is_set():
            raise NeuralTtsError('cancelled')
        if self.terminal or sequence != len(self.parts) or offset != self.size:
            raise ValueError('late_duplicate_or_out_of_order_chunk')
        if not pcm or len(pcm) % 2 or self.size + len(pcm) > self.limit:
            raise ValueError('invalid_chunk_size')
        self.parts.append(pcm)
        self.size += len(pcm)

    def finish(self, total, normal_end):
        if self.terminal or not normal_end or total != self.size or not total:
            raise ValueError('invalid_audio_end')
        self.terminal = 'completed'

    def cancel(self):
        if self.terminal is None:
            self.terminal = 'cancelled'


def cpu_checks():
    checks = []

    def rejected(label, operation):
        try:
            operation()
        except ValueError:
            checks.append(label)
            return
        raise AssertionError(label)

    stream = PcmChunks(12)
    stream.append(0, 0, b'\x01\x00' * 4)
    rejected('重复块被拒绝', lambda: stream.append(0, 0, b'\x02\x00'))
    rejected('缺失前块时拒绝后块', lambda: stream.append(2, 8, b'\x02\x00'))
    rejected('偏移不连续时拒绝', lambda: stream.append(1, 6, b'\x02\x00'))
    rejected('不完整采样被拒绝', lambda: stream.append(1, 8, b'\x02'))
    rejected('超过总长度上限被拒绝', lambda: stream.append(1, 8, b'\x02\x00' * 3))
    stream.append(1, 8, b'\x02\x00')
    rejected('没有正常结束通知不能成功', lambda: stream.finish(10, False))
    rejected('总长度不符不能成功', lambda: stream.finish(12, True))
    stream.finish(10, True)
    assert b''.join(stream.parts) == b'\x01\x00' * 4 + b'\x02\x00'
    checks.append('不足整块的尾音只拼接一次')
    rejected('结束后不追加声音', lambda: stream.append(2, 10, b'\x03\x00'))
    cancelled = PcmChunks()
    cancelled.append(0, 0, b'\x01\x00')
    cancelled.cancel()
    cancelled.cancel()
    rejected('取消后迟到块被拒绝', lambda: cancelled.append(1, 2, b'\x02\x00'))
    rejected('取消不能改成成功', lambda: cancelled.finish(2, True))
    during_decode = threading.Event()
    ready = PcmChunks()
    pcm = b'\x01\x00' * 4
    during_decode.set()
    try:
        ready.append(0, 0, pcm, cancel=during_decode)
    except NeuralTtsError as error:
        assert error.code == 'cancelled' and not ready.parts and ready.size == 0
        checks.append('解码期间取消后不追加已准备好的声音')
    else:
        raise AssertionError('解码期间取消后仍追加了声音')
    assert new_audio_span(120, 2, 6, 0) == (40, 120, 40)
    assert new_audio_span(160, 2, 8, 80) == (120, 160, 40)
    assert new_audio_span(80, 0, 4, 0) == (0, 80, 0)
    rejected('解码长度回退时停止', lambda: new_audio_span(80, 2, 4, 80))
    checks.append('参考音不进入输出，新增声音不重复')
    return checks


def preflight(config_path):
    config = load_voice_config(config_path)
    predictor_mode = config['voices']['female'].get('predictor_mode', 'official')
    if predictor_mode not in ('official', 'cuda_graph'):
        raise RuntimeError('女声预测方式不在当前已支持配置中')
    package = ROOT / 'Saved/AIResearch/TtsEvaluation/qwen/.venv/Lib/site-packages/qwen_tts'
    inference = package / 'inference/qwen3_tts_model.py'
    modeling = package / 'core/models/modeling_qwen3_tts.py'
    source = modeling.read_text(encoding='utf-8')
    if 'hidden_states=(outputs.hidden_states, codec_ids)' not in source or 'codec_ids = torch.cat((input_ids, predictor_result.sequences), dim=-1)' not in source:
        raise RuntimeError('本机 Talker 输出布局已变化，先检查源码再运行试验')
    return config, {'status': 'preflight_passed', 'cpu_checks': cpu_checks(), 'gpu_loaded': False,
                    'voice_identities': neural_voice_identities(config), 'config': config,
                    'female_predictor_mode': predictor_mode,
                    'source_sha256': {str(path.relative_to(ROOT)): sha(path.read_bytes()) for path in
                                      (inference, modeling, ROOT / 'Tools/DeliveryAI/neural_tts_worker.py')},
                    'script_sha256': sha(Path(__file__).read_bytes()), 'sources': SOURCES}


def require_idle_runtime():
    if os.name != 'nt':
        raise RuntimeError('本试验只使用当前已配置的 Windows 环境')
    result = subprocess.run(['powershell', '-NoProfile', '-Command',
        "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -or $_.Name -like 'python*' } | "
        "Select-Object ProcessId,Name,CommandLine | ConvertTo-Json -Compress"],
        capture_output=True, text=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    rows = json.loads(result.stdout) if result.stdout.strip() else []
    if isinstance(rows, dict):
        rows = [rows]
    server = str(ROOT / 'Tools/DeliveryAI/server.py').replace('\\', '/').lower()
    for row in rows:
        command = (row.get('CommandLine') or '').replace('\\', '/').lower()
        if row['Name'].lower().startswith('unrealeditor') or 'neural_tts_worker.py' in command or server in command:
            raise RuntimeError('编辑器或正式语音服务仍在运行，停止独立试验：' + str(row['ProcessId']))


class CodecTap:
    """观察官方 Talker 已产生的编码，不替换采样或模型输入"""
    def __init__(self, engine, task, chunk_size, started, cancel_after_chunks=None):
        self.engine, self.task = engine, task
        self.torch, self.np = engine.torch, engine.np
        self.chunk_size, self.started = chunk_size, started
        self.cancel_after_chunks = cancel_after_chunks
        self.codes, self.rows, self.audio = [], [], []
        self.ledger = PcmChunks(engine.config['max_pcm_bytes'])
        self.last_step = 0
        self.last_decoded_frame = 0
        self.reference_codes = None
        self.reference_frames = 0
        self.eos_observed_in_forward = False
        self.cancel_requested_at = None
        if hasattr(engine, 'prompt'):
            if len(engine.prompt) != 1 or not engine.prompt[0].icl_mode:
                raise RuntimeError('女 B 的固定参考必须保持单条 ICL 配置')
            self.reference_codes = engine.prompt[0].ref_code
            if self.reference_codes is None or self.reference_codes.ndim != 2 or self.reference_codes.shape[1] != 16:
                raise RuntimeError('参考声音编码布局不符')
            self.reference_frames = int(self.reference_codes.shape[0])

    def observe(self, _module, _args, _kwargs, output):
        if self.task.cancel.is_set():
            from neural_tts import NeuralTtsError
            raise NeuralTtsError('cancelled')
        hidden = output.hidden_states
        if not isinstance(hidden, tuple) or len(hidden) != 2:
            raise RuntimeError('Talker 不再提供完整编码，停止试验')
        ids = hidden[1]
        if ids is None:
            if self.codes:
                raise RuntimeError('同一句生成中途重新进入预填充')
            return
        step = int(output.generation_step)
        if tuple(ids.shape) != (1, 16) or step != self.last_step + 1:
            raise RuntimeError('编码步骤重复、缺失或形状变化')
        self.last_step = step
        frame = ids[0].detach().clone()
        eos = self.engine.model.model.config.talker_config.codec_eos_token_id
        if int(frame[0].item()) == eos:
            self.eos_observed_in_forward = True
            return
        if self.eos_observed_in_forward:
            raise RuntimeError('结束标记之后仍产生声音编码')
        self.codes.append(frame)
        if self.chunk_size and len(self.codes) - self.last_decoded_frame >= self.chunk_size:
            self.decode_prefix()

    def decode_prefix(self):
        if self.task.cancel.is_set() or self.ledger.terminal:
            raise RuntimeError('取消后不能继续解码')
        if len(self.codes) <= self.last_decoded_frame:
            raise RuntimeError('同一段编码不能重复解码并发送')
        started = time.perf_counter()
        codes = self.torch.stack(self.codes)
        if self.reference_codes is not None:
            codes = self.torch.cat((self.reference_codes.to(codes.device), codes), dim=0)
        # 先保留全部左上下文比较接缝；总长度仍受正式生成上限约束
        with self.torch.inference_mode():
            waves, rate = self.engine.model.model.speech_tokenizer.decode([{'audio_codes': codes}])
        self.engine.synchronize()
        if self.task.cancel.is_set():
            raise NeuralTtsError('cancelled')
        if len(waves) != 1 or rate != 24000:
            raise RuntimeError('分段解码返回了不同音频格式')
        waveform = waves[0]
        if hasattr(waveform, 'cpu'):
            waveform = waveform.detach().float().cpu().numpy()
        waveform = self.np.asarray(waveform, dtype=self.np.float32)
        if waveform.ndim != 1 or not self.np.isfinite(waveform).all():
            raise RuntimeError('分段解码返回了无效波形')
        start, end, cut = new_audio_span(len(waveform), self.reference_frames, len(codes), self.ledger.size // 2)
        segment = waveform[start:end].copy()
        if not segment.size:
            raise RuntimeError('新编码没有产生新的声音，不能伪造空块')
        previous = self.np.concatenate(self.audio) if self.audio else self.np.empty(0, dtype=self.np.float32)
        delta = waveform[cut:cut + len(previous)] - previous
        pcm = self.np.rint(self.np.clip(segment, -1, 1) * 32767).astype('<i2').tobytes()
        index = len(self.rows)
        self.ledger.append(index, self.ledger.size, pcm, cancel=self.task.cancel)
        self.audio.append(segment)
        self.last_decoded_frame = len(self.codes)
        self.rows.append({'sequence': index, 'codec_frames_so_far': len(self.codes),
                          'audio_ready_ms': (time.perf_counter() - self.started) * 1000,
                          'decode_ms': (time.perf_counter() - started) * 1000,
                          'reference_samples_removed': cut, 'samples': len(segment),
                          'already_emitted_prefix_max_difference': float(self.np.max(self.np.abs(delta))) if delta.size else 0,
                          'already_emitted_prefix_rms_difference': float(self.np.sqrt(self.np.mean(delta * delta))) if delta.size else 0,
                          'clipped_float_samples': int(self.np.count_nonzero(self.np.abs(segment) > 1)),
                          'pcm_sha256': sha(pcm)})
        if self.cancel_after_chunks and len(self.rows) >= self.cancel_after_chunks:
            self.cancel_requested_at = time.perf_counter()
            self.task.cancel.set()

    def codec_hash(self):
        if not self.codes:
            return None
        return sha(self.torch.stack(self.codes).cpu().numpy().astype('<i8').tobytes())


def run_sample(engines, voice, text, size, folder, name, cancel_after_chunks=None):
    engine = engines.engines[voice]
    task = SimpleNamespace(task_id=uuid.uuid4().hex, text=text, voice=voice,
                           speaker=engines.identities[voice]['speaker'], cancel=threading.Event())
    closed = threading.Event()
    started = time.perf_counter()
    tap = CodecTap(engine, task, size, started, cancel_after_chunks)
    hook = engine.model.model.talker.register_forward_hook(tap.observe, with_kwargs=True)
    row = {'name': name, 'voice': voice, 'text': text, 'chunk_codec_frames': size,
           'voice_identity': engines.identities[voice], 'status': 'started', 'chunks': tap.rows}
    try:
        message = engines.synthesize(task, closed)
        row['full_generation_ms'] = (time.perf_counter() - started) * 1000
        pcm = base64.b64decode(message['pcm'], validate=True)
        # synthesize 的现有实现已检查 EOS，达到 token 上限不作为正常结束
        if size:
            if tap.last_decoded_frame < len(tap.codes):
                tap.decode_prefix()
            if task.cancel.is_set():
                from neural_tts import NeuralTtsError
                raise NeuralTtsError('cancelled')
            tap.ledger.finish(len(pcm), normal_end=True)
            joined = b''.join(tap.ledger.parts)
            row['joined_audio'] = save_wav(folder / (name + '_chunks.wav'), joined)
            row['joined_matches_own_full_pcm'] = joined == pcm
            expected = engine.np.frombuffer(pcm, dtype='<i2').astype(engine.np.float32)
            actual = engine.np.frombuffer(joined, dtype='<i2').astype(engine.np.float32)
            row['pcm_max_absolute_difference'] = float(engine.np.max(engine.np.abs(expected - actual)))
            row['pcm_rms_difference'] = float(engine.np.sqrt(engine.np.mean((expected - actual) ** 2)))
            row['first_chunk_ready_ms'] = tap.rows[0]['audio_ready_ms']
            row['first_chunk_before_full_result'] = row['first_chunk_ready_ms'] < row['full_generation_ms']
            boundaries, offset = [], 0
            for part in tap.ledger.parts[:-1]:
                offset += len(part) // 2
                boundaries.append({'sample': offset, 'candidate_jump_pcm16': float(actual[offset] - actual[offset - 1]),
                                   'full_audio_jump_pcm16': float(expected[offset] - expected[offset - 1])})
            row['seams'] = boundaries
        # 尾块也准备好后再写 WAV，写盘时间不计入任何声音块的就绪时间
        row['full_audio'] = save_wav(folder / (name + '_full.wav'), pcm, message['sample_rate'])
        row['status'] = 'completed'
    except Exception as error:
        tap.ledger.cancel()
        row.update(status='cancelled' if getattr(error, 'code', None) == 'cancelled' else 'failed',
                   error=f'{type(error).__name__}: {error}')
    finally:
        hook.remove()
        # GPU 工作结束后才记录终态，下一次合成不会接着使用旧回调
        engine.synchronize()
        row['terminal_after_cuda_sync_ms'] = (time.perf_counter() - started) * 1000
        if tap.cancel_requested_at is not None:
            row['cancel_to_cuda_finished_ms'] = (time.perf_counter() - tap.cancel_requested_at) * 1000
        row['codec_sha256'] = tap.codec_hash()
        row['codec_frames'] = len(tap.codes)
        row['forward_eos_observed'] = tap.eos_observed_in_forward
        row['worker_eos_confirmed'] = row['status'] == 'completed'
        row['hook_removed'] = hook.id not in engine.model.model.talker._forward_hooks
        for i, part in enumerate(tap.ledger.parts):
            tap.rows[i].update(save_wav(folder / f'{name}_chunk_{i:03}.wav', part))
        save_json(folder / (name + '.json'), row)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', action='store_true', help='仅在正式服务与编辑器已停止后加载当前模型')
    parser.add_argument('--config', help='项目内配置路径；默认正式配置，沿用其中选择的女声预测方式')
    parser.add_argument('--voices', nargs='+', choices=('male', 'female'), default=['male', 'female'])
    parser.add_argument('--include-long', action='store_true', help='增加长句，默认每种声音只比较同一条短句')
    args = parser.parse_args()
    config, report = preflight(args.config)
    if not args.gpu:
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return
    require_idle_runtime()
    if Path(sys.executable).resolve() != project_path(config['python_path']):
        raise RuntimeError('GPU 试验必须使用正式配置中的独立 Python')
    from neural_tts_worker import QwenVoiceEngines
    folder = ROOT / 'Saved/AIResearch/TtsStreaming' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    folder.mkdir(parents=True, exist_ok=False)
    report.update(status='started', gpu_loaded=False, pid=os.getpid(), samples=[], subjectively_reviewed=False,
                  game_integration=False, timing_notes=[
                      '只有一个独立进程依次生成，没有与正式服务共享模型',
                      '基线也记录编码以便比较，保留生成参数；分段候选额外解码的成本计入总耗时',
                      '每种声音的首条完整基线可能包含首次解码开销，不据此推导整体加速倍数',
                      '分段就绪时间记录在内存 PCM 准备好时，WAV 写盘发生在本轮生成结束后',
                      '先使用累计前缀保留全部左上下文，尚未实现固定窗口或证明实机性能收益',
                      '取消后已产生的部分声音保留为诊断片段，不标记为完整台词'])
    manifest = folder / 'Manifest.json'
    save_json(manifest, report)
    closed = threading.Event()
    engines = None
    try:
        started = time.perf_counter()
        engines = QwenVoiceEngines(config, closed)
        report.update(gpu_loaded=True, model_prepare_ms=(time.perf_counter() - started) * 1000)
        for voice in dict.fromkeys(args.voices):
            cases = CASES if args.include_long else {'short': CASES['short']}
            for case, text in cases.items():
                baseline = run_sample(engines, voice, text, None, folder, f'{voice}_{case}_baseline')
                report['samples'].append(baseline)
                save_json(manifest, report)
                if baseline['status'] != 'completed':
                    raise RuntimeError('完整基线未正常结束，停止该试验')
                for size in (4, 8):
                    row = run_sample(engines, voice, text, size, folder, f'{voice}_{case}_chunks{size}')
                    row['codec_matches_baseline'] = row['codec_sha256'] == baseline['codec_sha256']
                    row['full_pcm_matches_baseline'] = row.get('full_audio', {}).get('pcm_sha256') == baseline['full_audio']['pcm_sha256']
                    report['samples'].append(row)
                    save_json(manifest, report)
                    if row['status'] != 'completed' or not row['codec_matches_baseline']:
                        raise RuntimeError('分段路径失败或改变了生成编码，保留结果并停止')
            cancelled = run_sample(engines, voice, CASES['short'], 4, folder, voice + '_cancel', cancel_after_chunks=1)
            report['samples'].append(cancelled)
            save_json(manifest, report)
            if cancelled['status'] != 'cancelled' or len(cancelled['chunks']) != 1 or not cancelled['hook_removed']:
                raise RuntimeError('首块之后取消未按预期结束')
            recovery = run_sample(engines, voice, CASES['short'], None, folder, voice + '_after_cancel')
            first = next(row for row in report['samples'] if row['name'] == voice + '_short_baseline')
            recovery['matches_before_cancel'] = recovery.get('full_audio', {}).get('pcm_sha256') == first['full_audio']['pcm_sha256']
            report['samples'].append(recovery)
            if recovery['status'] != 'completed' or not recovery['matches_before_cancel']:
                raise RuntimeError('取消后生成没有恢复到相同基线')
        candidates = [row for row in report['samples'] if row['status'] == 'completed' and row['chunk_codec_frames']]
        report['all_candidates_produced_pcm_before_full_result'] = bool(candidates) and all(
            row['first_chunk_before_full_result'] for row in candidates)
        report['all_candidates_match_full_pcm'] = bool(candidates) and all(
            row['joined_matches_own_full_pcm'] and row['full_pcm_matches_baseline'] for row in candidates)
        report['status'] = 'completed_pending_listening'
    except BaseException as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        closed.set()
        if engines:
            engines.synchronize()
        report['cuda_work_finished'] = engines is not None
        report['model_memory_release'] = '模型由本进程持有；脚本退出后释放，没有创建后台语音服务'
        save_json(manifest, report)
        print('RESULT_JSON=' + str(manifest), flush=True)


if __name__ == '__main__':
    main()
