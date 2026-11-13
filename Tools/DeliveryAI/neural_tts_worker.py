"""独立进程保留已配置的 Qwen 模型，接收线程处理取消，主线程依次生成声音"""
import base64
import argparse
from contextlib import contextmanager, redirect_stdout
import hashlib
import importlib
import importlib.metadata
import json
import os
from pathlib import Path
import queue
import re
import sys
import threading
import time
from types import SimpleNamespace
import wave

from neural_tts import (NeuralTtsError, load_voice_config, neural_voice_identities,
                        neural_voice_speakers, project_path)


VENDOR_HASHES = {
    'predictor_graph.py': '9f74155040fb6ac2381375331f76fb4efcd5b3c0761ebfc4977717223145cb76',
    'sampling.py': 'd52f136cb5d985f532239cb3f74ce5e8b9bfd269080e99295f09ce5f87bac988',
    'LICENSE': '442472a518bf71e371f2581aa0fcaf6ee2ef6854f78c340fdbe87c099950ea82',
    '__init__.py': 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
}
_torch_threads_initialized = False
_torch_threads_lock = threading.Lock()


def initialize_torch_threads(torch):
    global _torch_threads_initialized
    with _torch_threads_lock:
        if not _torch_threads_initialized:
            torch.set_num_threads(4)
            torch.set_num_interop_threads(1)
            _torch_threads_initialized = True


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def checked_clone_assets(entry):
    model_dir = project_path(entry['model_path'])
    manifest_path = project_path(entry['model_manifest_path'])
    if file_sha256(manifest_path) != entry['model_manifest_sha256']:
        raise NeuralTtsError('neural_tts_clone_manifest_changed')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    if (manifest.get('status') != 'completed' or manifest.get('revision') != entry['model_revision']
            or manifest.get('model_id') != 'Qwen/Qwen3-TTS-12Hz-0.6B-Base'
            or not isinstance(manifest.get('files'), list) or not manifest['files']):
        raise NeuralTtsError('neural_tts_clone_installation_incomplete')
    checked = set()
    for item in manifest['files']:
        if (not isinstance(item, dict) or not isinstance(item.get('path'), str)
                or type(item.get('bytes')) is not int or item['bytes'] <= 0
                or not isinstance(item.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', item['sha256'])):
            raise NeuralTtsError('neural_tts_clone_manifest_invalid')
        path = project_path(model_dir / item['path'])
        try:
            path.relative_to(model_dir)
        except ValueError:
            raise NeuralTtsError('neural_tts_clone_file_outside_model') from None
        if path in checked:
            raise NeuralTtsError('neural_tts_clone_manifest_invalid')
        checked.add(path)
        if not path.is_file() or path.stat().st_size != item['bytes'] or file_sha256(path) != item['sha256']:
            raise NeuralTtsError('neural_tts_clone_model_changed')
    if not {'config.json', 'model.safetensors', 'speech_tokenizer/model.safetensors'}.issubset(
            {item['path'] for item in manifest['files']}):
        raise NeuralTtsError('neural_tts_clone_manifest_invalid')
    reference = project_path(entry['reference_path'])
    if file_sha256(reference) != entry['reference_sha256']:
        raise NeuralTtsError('neural_tts_clone_reference_changed')
    with wave.open(str(reference), 'rb') as sound:
        if (sound.getnchannels() != 1 or sound.getsampwidth() != 2 or sound.getframerate() != 24000
                or sound.getcomptype() != 'NONE' or not 2400 <= sound.getnframes() <= 240000):
            raise NeuralTtsError('neural_tts_clone_reference_format')
    return model_dir, reference


def check_cancel(cancel, closed, deadline):
    if cancel.is_set() or closed.is_set():
        raise NeuralTtsError('cancelled')
    if time.monotonic() >= deadline:
        raise NeuralTtsError('neural_tts_timeout')


class WorkerRequests:
    def __init__(self, send, speakers):
        self.send = send
        self.speakers = speakers
        self.closed = threading.Event()
        self.lock = threading.Lock()
        self.pending = queue.Queue(maxsize=1)
        self.current = None

    def receive(self, message):
        if not isinstance(message, dict):
            raise ValueError('invalid_command')
        kind = message.get('type')
        if kind == 'close':
            self.close()
            return
        task_id = message.get('task_id')
        if not isinstance(task_id, str) or not re.fullmatch('[0-9a-f]{32}', task_id):
            raise ValueError('invalid_task_id')
        with self.lock:
            if kind == 'cancel':
                if self.current and self.current.task_id == task_id:
                    self.current.cancel.set()
                return
            if kind != 'synthesize':
                raise ValueError('invalid_command')
            text = message.get('text')
            voice = message.get('voice')
            if (not isinstance(text, str) or not text.strip() or len(text) > 120
                    or not isinstance(voice, str) or voice not in self.speakers
                    or message.get('emotion') not in ('Calm', 'Angry', 'Surprised')):
                self.send({'type': 'error', 'task_id': task_id, 'code': 'neural_tts_invalid_speech'})
                return
            if self.closed.is_set() or self.current:
                self.send({'type': 'error', 'task_id': task_id, 'code': 'neural_tts_busy'})
                return
            self.current = SimpleNamespace(task_id=task_id, text=text, voice=voice, speaker=self.speakers[voice],
                                           cancel=threading.Event())
            self.pending.put_nowait(self.current)

    def finish(self, task):
        with self.lock:
            if self.current is task:
                self.current = None

    def close(self):
        self.closed.set()
        with self.lock:
            if self.current:
                self.current.cancel.set()


@contextmanager
def generation_controls(engine, cancel, closed, deadline):
    """替换当前实例的方法，取消时抛出异常，完整音频只在正常结束后返回"""
    torch, talker = engine.torch, engine.model.model.talker
    predictor = talker.code_predictor
    original_talker = talker.generate
    originals = [(owner, 'generate' in vars(owner), vars(owner).get('generate')) for owner in (talker, predictor)]
    expected = {'max_new_tokens': 15, 'do_sample': True, 'top_p': 1.0, 'top_k': 50,
                'temperature': .9, 'output_hidden_states': True, 'return_dict_in_generate': True}

    def predict(*args, **kwargs):
        check_cancel(cancel, closed, deadline)
        if args or set(kwargs) != {'inputs_embeds', *expected}:
            raise NeuralTtsError('neural_tts_predictor_interface_changed')
        if any(kwargs[key] != value for key, value in expected.items()):
            raise NeuralTtsError('neural_tts_predictor_settings_changed')
        inputs = kwargs['inputs_embeds']
        if (tuple(inputs.shape) != (1, 2, engine.hidden_size) or inputs.dtype != torch.bfloat16
                or inputs.device != torch.device('cuda:0')):
            raise NeuralTtsError('neural_tts_predictor_input_changed')
        tokens = engine.graph.run(inputs)
        return SimpleNamespace(sequences=tokens.unsqueeze(0))

    class CancellationCriterion(engine.StoppingCriteria):
        def __call__(self, input_ids, scores, **kwargs):
            check_cancel(cancel, closed, deadline)
            return False

    def generate(*args, **kwargs):
        check_cancel(cancel, closed, deadline)
        criteria = list(kwargs.get('stopping_criteria') or [])
        kwargs['stopping_criteria'] = engine.StoppingCriteriaList([*criteria, CancellationCriterion()])
        output = original_talker(*args, **kwargs)
        check_cancel(cancel, closed, deadline)
        # 达到长度上限但没有正常结束时报告失败，避免把截断的台词当成完整声音
        eos = engine.model.model.config.talker_config.codec_eos_token_id
        if not bool((output.sequences == eos).any().item()):
            raise NeuralTtsError('neural_tts_token_limit')
        return output

    talker.generate, predictor.generate = generate, predict
    try:
        yield
    finally:
        for owner, had_value, value in originals:
            if had_value:
                owner.generate = value
            else:
                delattr(owner, 'generate')


def capture_predictor_graph(model, torch, stage):
    """按同一套采样设置，为当前模型建立独立的 Graph"""
    predictor = model.model.talker.code_predictor
    hidden_size = model.model.config.talker_config.hidden_size
    if model.model.config.talker_config.num_code_groups != 16:
        raise NeuralTtsError('neural_tts_code_groups_changed')
    vendor = Path(__file__).with_name('vendor') / 'faster_qwen_v032'
    for name, digest in VENDOR_HASHES.items():
        if hashlib.sha256((vendor / name).read_bytes()).hexdigest() != digest:
            raise NeuralTtsError('neural_tts_vendor_changed')
    sys.path.insert(0, str(vendor.parent))
    try:
        module = importlib.import_module('faster_qwen_v032.predictor_graph')
    finally:
        sys.path.remove(str(vendor.parent))
    if Path(module.__file__).resolve() != (vendor / 'predictor_graph.py').resolve():
        raise NeuralTtsError('neural_tts_wrong_vendor')
    original_sample = module.sample_logits

    def sample(logits, **kwargs):
        return original_sample(logits.float(), **kwargs)

    # 录图时使用与原 HF 生成相同的 float32 采样精度
    module.sample_logits = sample
    try:
        stage('capturing_predictor_graph')
        graph = module.PredictorGraph(
            predictor, predictor.model.config, hidden_size, device='cuda:0',
            dtype=torch.bfloat16, do_sample=True, top_k=50, top_p=1.0, temperature=.9)
        graph.capture(num_warmup=3)
    finally:
        module.sample_logits = original_sample
    return graph, hidden_size


class QwenEngine:
    def __init__(self, config, closed, check_startup=False):
        def stage(name):
            print(f'qwen startup={name}', file=sys.stderr, flush=True)
        stage('checking_environment')
        self.config = config
        executable = project_path(config['python_path'])
        if Path(sys.executable).resolve() != executable:
            raise NeuralTtsError('neural_tts_wrong_environment')
        if config['cpu_threads'] != 4 or config['max_new_tokens'] != 256 or config['seed'] != 20261113:
            raise NeuralTtsError('neural_tts_settings_changed')
        installation = json.loads(project_path(config['installation_path']).read_text(encoding='utf-8-sig'))
        if not installation.get('completed_at') or installation.get('model_revision') != config['model_revision']:
            raise NeuralTtsError('neural_tts_installation_incomplete')
        model_dir = project_path(config['model_path'])
        downloads = json.loads((model_dir.parent / 'model-downloads.json').read_text(encoding='utf-8-sig'))
        if downloads.get('revision') != config['model_revision']:
            raise NeuralTtsError('neural_tts_model_revision_changed')
        for entry in downloads['files']:
            path = project_path(str(Path(config['model_path']) / entry['path']))
            if not path.is_file() or path.stat().st_size != entry['bytes']:
                raise NeuralTtsError('neural_tts_model_file_changed')
        for name, version in config['packages'].items():
            if importlib.metadata.version(name) != version:
                raise NeuralTtsError('neural_tts_package_version_changed')
        stage('environment_checked')
        for name in ('OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'OPENBLAS_NUM_THREADS'):
            os.environ[name] = '4'
        os.environ.update({'HF_HUB_OFFLINE': '1', 'TRANSFORMERS_OFFLINE': '1',
                           'HF_HUB_DISABLE_TELEMETRY': '1', 'TOKENIZERS_PARALLELISM': 'false',
                           'GRADIO_ANALYTICS_ENABLED': 'False'})

        def reject_network(event, _args):
            if event in ('socket.connect', 'socket.getaddrinfo'):
                raise NeuralTtsError('neural_tts_network_disabled')
        sys.addaudithook(reject_network)
        stage('importing_torch')
        import torch
        stage('importing_numpy')
        import numpy as np
        stage('importing_qwen')
        from qwen_tts import Qwen3TTSModel
        from transformers import StoppingCriteria, StoppingCriteriaList
        stage('imports_completed')
        self.torch, self.np = torch, np
        self.ModelClass = Qwen3TTSModel
        self.StoppingCriteria, self.StoppingCriteriaList = StoppingCriteria, StoppingCriteriaList
        if check_startup:
            return
        initialize_torch_threads(torch)
        if not torch.cuda.is_available():
            raise NeuralTtsError('neural_tts_cuda_unavailable')
        torch.cuda.set_device(0)
        check_cancel(closed, closed, float('inf'))
        stage('loading_model')
        self.model = Qwen3TTSModel.from_pretrained(
            str(model_dir), device_map='cuda:0', dtype=torch.bfloat16,
            attn_implementation='sdpa', local_files_only=True)
        stage('model_loaded')
        available_speakers = {name.lower() for name in self.model.get_supported_speakers()}
        if any(entry['speaker'].lower() not in available_speakers
               for entry in config['voices'].values() if entry['provider'] == 'qwen'):
            raise NeuralTtsError('neural_tts_speaker_unavailable')
        check_cancel(closed, closed, float('inf'))
        self.graph, self.hidden_size = capture_predictor_graph(self.model, torch, stage)
        torch.cuda.synchronize()
        check_cancel(closed, closed, float('inf'))
        stage('ready')

    def synchronize(self):
        self.torch.cuda.synchronize()

    def synthesize(self, task, closed):
        torch, np = self.torch, self.np
        deadline = time.monotonic() + self.config['synthesis_timeout_seconds']
        check_cancel(task.cancel, closed, deadline)
        torch.manual_seed(self.config['seed'])
        torch.cuda.manual_seed_all(self.config['seed'])
        with generation_controls(self, task.cancel, closed, deadline), torch.inference_mode():
            wavs, rate = self.model.generate_custom_voice(
                text=task.text, language='Chinese', speaker=task.speaker, non_streaming_mode=True,
                max_new_tokens=256, do_sample=True, top_k=50, top_p=1.0, temperature=.9,
                repetition_penalty=1.05, subtalker_dosample=True, subtalker_top_k=50,
                subtalker_top_p=1.0, subtalker_temperature=.9)
        self.synchronize()
        check_cancel(task.cancel, closed, deadline)
        if rate != 24000 or len(wavs) != 1:
            raise NeuralTtsError('neural_tts_invalid_audio_format')
        values = np.asarray(wavs[0], dtype=np.float32)
        if values.ndim != 1 or not values.size or not np.isfinite(values).all():
            raise NeuralTtsError('neural_tts_invalid_pcm')
        if values.size * 2 > self.config['max_pcm_bytes']:
            raise NeuralTtsError('neural_tts_audio_too_long')
        clipped = int(np.count_nonzero(np.abs(values) > 1.0))
        pcm = np.rint(np.clip(values, -1.0, 1.0) * 32767).astype('<i2').tobytes()
        return {'type': 'audio', 'task_id': task.task_id, 'pcm': base64.b64encode(pcm).decode('ascii'),
                'sample_rate': 24000, 'voice_name': task.speaker, 'frames': int(values.size),
                'clipped_samples': clipped}


@contextmanager
def clone_generation_controls(engine, cancel, closed, deadline):
    """Base 保留官方采样，仅在生成步骤间检查取消和正常结束"""
    talker = engine.model.model.talker
    had_value, previous = 'generate' in vars(talker), vars(talker).get('generate')
    original = talker.generate

    class CancellationCriterion(engine.StoppingCriteria):
        def __call__(self, input_ids, scores, **kwargs):
            check_cancel(cancel, closed, deadline)
            return False

    def generate(*args, **kwargs):
        check_cancel(cancel, closed, deadline)
        kwargs['stopping_criteria'] = engine.StoppingCriteriaList(
            [*(kwargs.get('stopping_criteria') or []), CancellationCriterion()])
        output = original(*args, **kwargs)
        check_cancel(cancel, closed, deadline)
        eos = engine.model.model.config.talker_config.codec_eos_token_id
        if not bool((output.sequences == eos).any().item()):
            raise NeuralTtsError('neural_tts_token_limit')
        return output

    talker.generate = generate
    try:
        yield
    finally:
        if had_value:
            talker.generate = previous
        else:
            delattr(talker, 'generate')


class QwenCloneEngine:
    """女声 Base 模型常驻，固定参考只在启动时建立一次"""
    def __init__(self, config, closed, runtime, check_startup=False):
        self.config = config
        self.entry = config['voices']['female']
        self.predictor_mode = self.entry.get('predictor_mode', 'official')
        if self.predictor_mode not in ('official', 'cuda_graph'):
            raise NeuralTtsError('neural_tts_invalid_predictor_mode')
        self.torch, self.np = runtime.torch, runtime.np
        self.StoppingCriteria, self.StoppingCriteriaList = runtime.StoppingCriteria, runtime.StoppingCriteriaList
        model_dir, reference = checked_clone_assets(self.entry)
        if check_startup:
            return
        check_cancel(closed, closed, float('inf'))
        print('qwen startup=loading_female_base', file=sys.stderr, flush=True)
        self.model = runtime.ModelClass.from_pretrained(
            str(model_dir), device_map='cuda:0', dtype=self.torch.bfloat16,
            attn_implementation='sdpa', local_files_only=True)
        if self.model.model.tts_model_type != 'base':
            raise NeuralTtsError('neural_tts_clone_wrong_model')
        check_cancel(closed, closed, float('inf'))
        self.torch.manual_seed(config['seed'])
        self.torch.cuda.manual_seed_all(config['seed'])
        with self.torch.inference_mode():
            self.prompt = self.model.create_voice_clone_prompt(
                ref_audio=str(reference), ref_text=self.entry['reference_text'], x_vector_only_mode=False)
        self.synchronize()
        check_cancel(closed, closed, float('inf'))
        if not self.prompt:
            raise NeuralTtsError('neural_tts_clone_reference_empty')
        print('qwen startup=female_reference_ready', file=sys.stderr, flush=True)
        if self.predictor_mode == 'cuda_graph':
            def stage(name):
                print(f'qwen startup=female_{name}', file=sys.stderr, flush=True)
            self.graph, self.hidden_size = capture_predictor_graph(self.model, self.torch, stage)
            self.synchronize()
            check_cancel(closed, closed, float('inf'))

    def synchronize(self):
        self.torch.cuda.synchronize()

    def synthesize(self, task, closed):
        torch, np = self.torch, self.np
        deadline = time.monotonic() + self.config['synthesis_timeout_seconds']
        check_cancel(task.cancel, closed, deadline)
        torch.manual_seed(self.config['seed'])
        torch.cuda.manual_seed_all(self.config['seed'])
        # Graph 只在配置明确选择时使用；取消或异常后，两条路径都会还原模型方法
        controls = generation_controls if self.predictor_mode == 'cuda_graph' else clone_generation_controls
        with controls(self, task.cancel, closed, deadline), torch.inference_mode():
            wavs, rate = self.model.generate_voice_clone(
                text=task.text, language='Chinese', voice_clone_prompt=self.prompt, non_streaming_mode=True,
                max_new_tokens=256, do_sample=True, top_k=50, top_p=1.0, temperature=.9,
                repetition_penalty=1.05, subtalker_dosample=True, subtalker_top_k=50,
                subtalker_top_p=1.0, subtalker_temperature=.9)
        self.synchronize()
        check_cancel(task.cancel, closed, deadline)
        if rate != 24000 or len(wavs) != 1:
            raise NeuralTtsError('neural_tts_invalid_audio_format')
        values = np.asarray(wavs[0], dtype=np.float32)
        if values.ndim != 1 or not values.size or not np.isfinite(values).all():
            raise NeuralTtsError('neural_tts_invalid_pcm')
        if values.size * 2 > self.config['max_pcm_bytes']:
            raise NeuralTtsError('neural_tts_audio_too_long')
        clipped = int(np.count_nonzero(np.abs(values) > 1.0))
        pcm = np.rint(np.clip(values, -1.0, 1.0) * 32767).astype('<i2').tobytes()
        return {'type': 'audio', 'task_id': task.task_id, 'pcm': base64.b64encode(pcm).decode('ascii'),
                'sample_rate': 24000, 'voice_name': self.entry['speaker'], 'frames': int(values.size),
                'clipped_samples': clipped}


class QwenVoiceEngines:
    """两类声音共用现有单任务队列，轮到哪位 NPC 就使用已加载的对应模型"""
    def __init__(self, config, closed, check_startup=False):
        self.custom = QwenEngine(config, closed, check_startup=check_startup)
        self.identities = neural_voice_identities(config)
        self.engines = {voice: self.custom for voice, entry in config['voices'].items() if entry['provider'] == 'qwen'}
        if config['voices']['female']['provider'] == 'qwen_base':
            self.engines['female'] = QwenCloneEngine(config, closed, self.custom, check_startup=check_startup)

    def synthesize(self, task, closed):
        engine = self.engines.get(task.voice)
        if engine is None or task.speaker != self.identities[task.voice]['speaker']:
            raise NeuralTtsError('neural_tts_invalid_speech')
        result = engine.synthesize(task, closed)
        result['voice_identity'] = self.identities[task.voice]
        return result

    def synchronize(self):
        # 同一 CUDA 设备上的两份模型都确认结束后，才能通知主后端开始下一句
        self.custom.synchronize()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-startup', action='store_true', help='仅检查环境和导入，不加载模型或使用 CUDA')
    parser.add_argument('--config', help='使用项目目录内的语音配置，省略时读取正式配置')
    args = parser.parse_args()
    protocol_output = sys.stdout
    output_lock = threading.Lock()

    def send(message):
        with output_lock:
            protocol_output.write(json.dumps(message, ensure_ascii=False, separators=(',', ':')) + '\n')
            protocol_output.flush()

    config = load_voice_config(args.config)
    speakers = neural_voice_speakers(config)
    requests = WorkerRequests(send, speakers)

    def read_input():
        try:
            while not requests.closed.is_set():
                line = sys.stdin.readline(8193)
                if not line:
                    break
                if len(line) > 8192 or not line.endswith('\n'):
                    raise ValueError('command_too_large')
                requests.receive(json.loads(line))
        except (OSError, ValueError, queue.Full):
            pass
        finally:
            requests.close()

    input_thread = threading.Thread(target=read_input, name='qwen-speech-input', daemon=True)
    try:
        if args.check_startup:
            import faulthandler
            faulthandler.dump_traceback_later(12, file=sys.stderr)
        # 库的初始化日志和录图提示写入 stderr，stdout 始终只发送协议消息
        with redirect_stdout(sys.stderr):
            engine = QwenVoiceEngines(config, requests.closed, check_startup=args.check_startup)
        # Windows 加载 NumPy 扩展时会访问标准输入锁，模型初始化后再启动阻塞读取
        input_thread.start()
        if args.check_startup:
            faulthandler.cancel_dump_traceback_later()
            send({'type': 'startup_checked', 'imports_only': True})
            requests.closed.wait(20)
            return 0
        send({'type': 'ready', 'protocol_version': 1, 'voice_name': 'Uncle_Fu',
              'voices': speakers, 'voice_identities': neural_voice_identities(config), 'sample_rate': 24000})
        while not requests.closed.is_set() or not requests.pending.empty():
            try:
                task = requests.pending.get(timeout=.1)
            except queue.Empty:
                continue
            started = time.monotonic()
            try:
                with redirect_stdout(sys.stderr):
                    message = engine.synthesize(task, requests.closed)
            except Exception as error:
                code = error.code if isinstance(error, NeuralTtsError) else 'neural_tts_generation_failed'
                print(f'qwen task={task.task_id} failed={code} exception={type(error).__name__}', file=sys.stderr, flush=True)
                message = {'type': 'cancelled' if code == 'cancelled' else 'error', 'task_id': task.task_id, 'code': code}
            finally:
                # 等待已提交的 GPU 工作结束，再通知父进程本任务已经结束
                engine.synchronize()
                requests.finish(task)
            if task.cancel.is_set() or requests.closed.is_set():
                message = {'type': 'cancelled', 'task_id': task.task_id, 'code': 'cancelled'}
            message['elapsed_ms'] = int((time.monotonic() - started) * 1000)
            send(message)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        code = error.code if isinstance(error, NeuralTtsError) else 'neural_tts_worker_failed'
        print(f'qwen worker failed={code} exception={type(error).__name__}', file=sys.stderr, flush=True)
        try:
            send({'type': 'error', 'code': code})
        except OSError:
            pass
        return 1
    finally:
        requests.close()


if __name__ == '__main__':
    raise SystemExit(main())
