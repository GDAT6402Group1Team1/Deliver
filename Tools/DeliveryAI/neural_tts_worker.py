"""独立进程只加载一份 Qwen 模型，接收线程处理取消，主线程依次生成声音"""
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

from neural_tts import NeuralTtsError, load_voice_config, project_path


VENDOR_HASHES = {
    'predictor_graph.py': '9f74155040fb6ac2381375331f76fb4efcd5b3c0761ebfc4977717223145cb76',
    'sampling.py': 'd52f136cb5d985f532239cb3f74ce5e8b9bfd269080e99295f09ce5f87bac988',
    'LICENSE': '442472a518bf71e371f2581aa0fcaf6ee2ef6854f78c340fdbe87c099950ea82',
    '__init__.py': 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
}


def check_cancel(cancel, closed, deadline):
    if cancel.is_set() or closed.is_set():
        raise NeuralTtsError('cancelled')
    if time.monotonic() >= deadline:
        raise NeuralTtsError('neural_tts_timeout')


class WorkerRequests:
    def __init__(self, send):
        self.send = send
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
            if (not isinstance(text, str) or not text.strip() or len(text) > 120
                    or message.get('voice') != 'male' or message.get('emotion') not in ('Calm', 'Angry', 'Surprised')):
                self.send({'type': 'error', 'task_id': task_id, 'code': 'neural_tts_invalid_speech'})
                return
            if self.closed.is_set() or self.current:
                self.send({'type': 'error', 'task_id': task_id, 'code': 'neural_tts_busy'})
                return
            self.current = SimpleNamespace(task_id=task_id, text=text, cancel=threading.Event())
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
        self.StoppingCriteria, self.StoppingCriteriaList = StoppingCriteria, StoppingCriteriaList
        if check_startup:
            return
        torch.set_num_threads(4)
        torch.set_num_interop_threads(1)
        if not torch.cuda.is_available():
            raise NeuralTtsError('neural_tts_cuda_unavailable')
        torch.cuda.set_device(0)
        check_cancel(closed, closed, float('inf'))
        stage('loading_model')
        self.model = Qwen3TTSModel.from_pretrained(
            str(model_dir), device_map='cuda:0', dtype=torch.bfloat16,
            attn_implementation='sdpa', local_files_only=True)
        stage('model_loaded')
        check_cancel(closed, closed, float('inf'))
        predictor = self.model.model.talker.code_predictor
        self.hidden_size = self.model.model.config.talker_config.hidden_size
        if self.model.model.config.talker_config.num_code_groups != 16:
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
            self.graph = module.PredictorGraph(
                predictor, predictor.model.config, self.hidden_size, device='cuda:0',
                dtype=torch.bfloat16, do_sample=True, top_k=50, top_p=1.0, temperature=.9)
            self.graph.capture(num_warmup=3)
        finally:
            module.sample_logits = original_sample
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
                text=task.text, language='Chinese', speaker='Uncle_Fu', non_streaming_mode=True,
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
                'sample_rate': 24000, 'voice_name': 'Uncle_Fu', 'frames': int(values.size),
                'clipped_samples': clipped}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-startup', action='store_true', help='仅检查环境和导入，不加载模型或使用 CUDA')
    args = parser.parse_args()
    protocol_output = sys.stdout
    output_lock = threading.Lock()

    def send(message):
        with output_lock:
            protocol_output.write(json.dumps(message, ensure_ascii=False, separators=(',', ':')) + '\n')
            protocol_output.flush()

    requests = WorkerRequests(send)

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
            engine = QwenEngine(load_voice_config(), requests.closed, check_startup=args.check_startup)
        # Windows 加载 NumPy 扩展时会访问标准输入锁，模型初始化后再启动阻塞读取
        input_thread.start()
        if args.check_startup:
            faulthandler.cancel_dump_traceback_later()
            send({'type': 'startup_checked', 'imports_only': True})
            requests.closed.wait(20)
            return 0
        send({'type': 'ready', 'protocol_version': 1, 'voice_name': 'Uncle_Fu', 'sample_rate': 24000})
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
