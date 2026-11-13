"""通过独立 Python 进程合成 Qwen 语音，主后端不加载模型或 CUDA"""
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import stat
import subprocess
import threading
import time
import uuid


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = Path(__file__).with_name('neural_voice_config.json')
MAX_PROTOCOL_LINE = 524288


class NeuralTtsError(RuntimeError):
    def __init__(self, code):
        self.code = code
        super().__init__(code)


def project_path(relative):
    path = PROJECT_ROOT / relative
    path.absolute().relative_to(PROJECT_ROOT)
    for current in (path, *path.parents):
        if current.exists() or current.is_symlink():
            flags = getattr(current.lstat(), 'st_file_attributes', 0)
            if current.is_symlink() or flags & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 1024):
                raise NeuralTtsError('neural_tts_linked_path')
        if current == PROJECT_ROOT:
            break
    path.resolve().relative_to(PROJECT_ROOT)
    return path.resolve()


def load_voice_config(config_path=None):
    config = json.loads(project_path(config_path or CONFIG_PATH).read_text(encoding='utf-8-sig'))
    if (not isinstance(config, dict) or type(config.get('schema_version')) is not int
            or config.get('schema_version') != 1 or config.get('sample_rate') != 24000
            or config.get('max_pcm_bytes') != 384000 or config.get('max_text_chars') != 120):
        raise NeuralTtsError('neural_tts_invalid_config')
    voices = config.get('voices')
    if (not isinstance(voices, dict) or set(voices) != {'male', 'female'}
            or voices['male'] != {'provider': 'qwen', 'speaker': 'Uncle_Fu'}):
        raise NeuralTtsError('neural_tts_invalid_voices')
    female = voices['female']
    if isinstance(female, dict) and female.get('provider') == 'qwen_base':
        fields = {'provider', 'speaker', 'model_path', 'model_revision', 'model_manifest_path',
                  'model_manifest_sha256', 'reference_path', 'reference_sha256', 'reference_text'}
        if (set(female) != fields or female['speaker'] != 'Xiaofang_B'
                or not isinstance(female['reference_text'], str)
                or not 1 <= len(female['reference_text'].strip()) <= 120):
            raise NeuralTtsError('neural_tts_invalid_voices')
        for key, length in (('model_revision', 40), ('model_manifest_sha256', 64), ('reference_sha256', 64)):
            if not isinstance(female[key], str) or not re.fullmatch('[0-9a-f]{'+str(length)+'}', female[key]):
                raise NeuralTtsError('neural_tts_invalid_voices')
        for key in ('model_path', 'model_manifest_path', 'reference_path'):
            if not isinstance(female[key], str) or not female[key]:
                raise NeuralTtsError('neural_tts_invalid_voices')
            project_path(female[key])
    elif female not in ({'provider': 'windows'}, {'provider': 'qwen', 'speaker': 'Serena'},
                         {'provider': 'qwen', 'speaker': 'Vivian'}):
        raise NeuralTtsError('neural_tts_invalid_voices')
    for key in ('python_path', 'model_path', 'installation_path'):
        project_path(config[key])
    for key in ('startup_timeout_seconds', 'synthesis_timeout_seconds', 'cancel_grace_seconds',
                'process_exit_timeout_seconds'):
        if type(config.get(key)) not in (int, float) or not 0 < config[key] <= 120:
            raise NeuralTtsError('neural_tts_invalid_timeout')
    return config


def neural_voice_speakers(config):
    return {voice: entry['speaker'] for voice, entry in config['voices'].items()
            if entry['provider'] in ('qwen', 'qwen_base')}


def neural_voice_identities(config):
    identities = {}
    for voice, entry in config['voices'].items():
        if entry['provider'] not in ('qwen', 'qwen_base'):
            continue
        identity = {'provider': entry['provider'], 'speaker': entry['speaker'],
                    'model_revision': entry.get('model_revision', config['model_revision'])}
        if entry['provider'] == 'qwen_base':
            identity.update(reference_sha256=entry['reference_sha256'],
                            reference_text_sha256=hashlib.sha256(entry['reference_text'].encode('utf-8')).hexdigest())
        identities[voice] = identity
    return identities


def uses_neural_voice(voice):
    # 只有明确启用 Qwen 时才接入，普通测试和 fixture 不启动模型
    if os.environ.get('DELIVERY_TTS_PROVIDER', '').lower() != 'qwen':
        return False
    return load_voice_config()['voices'].get(voice, {}).get('provider') in ('qwen', 'qwen_base')


class QwenSpeechSynthesizer:
    """后端共享一个实例，GPU 任务由调用方按玩家优先顺序提交"""
    def __init__(self, config_path=None):
        self.config_path = project_path(config_path or CONFIG_PATH)
        self.config = load_voice_config(self.config_path)
        self.speakers = neural_voice_speakers(self.config)
        self.identities = neural_voice_identities(self.config)
        self._completed_voice = threading.local()
        self.process = None
        self.ready = False
        self.closed = threading.Event()
        self.lock = threading.Lock()
        self.messages = queue.Queue(maxsize=8)
        self.reader_failed = threading.Event()
        self.stderr_file = None
        self.stopping_failed = False

    @property
    def voice_name(self):
        # 每个调用线程记录自己的结果，男女同时排队时日志不会串音色
        return getattr(self._completed_voice, 'name', self.speakers['male'])

    def _acquire(self, cancel):
        while True:
            if self.closed.is_set() or cancel.is_set():
                raise NeuralTtsError('cancelled')
            if self.lock.acquire(timeout=.05):
                return

    def _send(self, message):
        self.process.stdin.write(json.dumps(message, ensure_ascii=False) + '\n')
        self.process.stdin.flush()

    def _stop_owned_process(self):
        process = self.process
        if process is None:
            return
        self.stopping_failed = True
        if process.poll() is None:
            process.kill()
        # 确认这个子进程退出后才能创建下一份模型
        process.wait(timeout=self.config['process_exit_timeout_seconds'])
        for pipe in (process.stdin, process.stdout):
            if pipe:
                pipe.close()
        if self.stderr_file:
            self.stderr_file.close()
            self.stderr_file = None
        self.process = None
        self.ready = False
        self.stopping_failed = False

    def _start(self):
        if self.stopping_failed:
            self._stop_owned_process()
        if self.process and self.process.poll() is None:
            return
        if self.process:
            self._stop_owned_process()
        executable = project_path(self.config['python_path'])
        if not executable.is_file():
            raise NeuralTtsError('neural_tts_python_missing')
        self.messages = queue.Queue(maxsize=8)
        self.reader_failed = threading.Event()
        environment = dict(os.environ)
        environment.update({'PYTHONIOENCODING': 'utf-8', 'PYTHONUNBUFFERED': '1',
                            'PYTHONDONTWRITEBYTECODE': '1', 'HF_HUB_OFFLINE': '1',
                            'TRANSFORMERS_OFFLINE': '1', 'HF_HUB_DISABLE_TELEMETRY': '1'})
        log_path = project_path('Saved/Logs/QwenSpeechWorker.log')
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self.stderr_file = log_path.open('a', encoding='utf-8')
        try:
            self.process = subprocess.Popen(
                [str(executable), '-B', '-X', 'utf8', '-u', str(Path(__file__).with_name('neural_tts_worker.py')),
                 '--config', str(self.config_path)],
                cwd=str(PROJECT_ROOT), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr_file,
                text=True, encoding='utf-8', env=environment,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except Exception:
            self.stderr_file.close()
            self.stderr_file = None
            raise
        process, messages, failed = self.process, self.messages, self.reader_failed

        def read_output():
            try:
                while True:
                    line = process.stdout.readline(MAX_PROTOCOL_LINE + 1)
                    if not line:
                        return
                    if len(line) > MAX_PROTOCOL_LINE or not line.endswith('\n'):
                        raise ValueError('oversized_worker_message')
                    value = json.loads(line)
                    if not isinstance(value, dict):
                        raise ValueError('invalid_worker_message')
                    messages.put_nowait(value)
            except (OSError, ValueError, queue.Full):
                failed.set()

        threading.Thread(target=read_output, name='qwen-speech-output', daemon=True).start()

    def _receive(self):
        if self.reader_failed.is_set():
            raise NeuralTtsError('neural_tts_invalid_worker_output')
        try:
            return self.messages.get(timeout=.05)
        except queue.Empty:
            if self.process.poll() is not None:
                raise NeuralTtsError('neural_tts_worker_exited')
            return None

    def _prepare_locked(self, cancel):
        if self.closed.is_set() or cancel.is_set():
            raise NeuralTtsError('cancelled')
        self._start()
        if self.ready:
            return True
        deadline = time.monotonic() + self.config['startup_timeout_seconds']
        try:
            while True:
                if self.closed.is_set() or cancel.is_set():
                    raise NeuralTtsError('cancelled')
                if time.monotonic() >= deadline:
                    raise NeuralTtsError('neural_tts_prepare_timeout')
                message = self._receive()
                if message is None:
                    continue
                if (message.get('type') != 'ready' or message.get('protocol_version') != 1
                        or message.get('voice_name') != 'Uncle_Fu' or message.get('sample_rate') != 24000
                        or message.get('voices') != self.speakers
                        or message.get('voice_identities') != self.identities):
                    raise NeuralTtsError(message.get('code', 'neural_tts_prepare_failed'))
                self.ready = True
                return True
        except BaseException:
            self._stop_owned_process()
            raise

    def prepare(self, cancel):
        self._acquire(cancel)
        try:
            return self._prepare_locked(cancel)
        finally:
            self.lock.release()

    def synthesize(self, text, voice, emotion, cancel):
        if (not isinstance(text, str) or not text.strip() or len(text) > 120
                or not isinstance(voice, str) or voice not in self.speakers):
            raise NeuralTtsError('neural_tts_invalid_speech')
        if emotion not in ('Calm', 'Angry', 'Surprised'):
            raise NeuralTtsError('neural_tts_invalid_emotion')
        self._acquire(cancel)
        try:
            self._prepare_locked(cancel)
            task_id = uuid.uuid4().hex
            self._send({'type': 'synthesize', 'task_id': task_id, 'text': text, 'voice': voice, 'emotion': emotion})
            deadline = time.monotonic() + self.config['synthesis_timeout_seconds']
            abort_reason = None
            while True:
                if abort_reason is None and (self.closed.is_set() or cancel.is_set() or time.monotonic() >= deadline):
                    abort_reason = 'cancelled' if self.closed.is_set() or cancel.is_set() else 'neural_tts_timeout'
                    self._send({'type': 'cancel', 'task_id': task_id})
                    deadline = time.monotonic() + self.config['cancel_grace_seconds']
                if abort_reason is not None and time.monotonic() >= deadline:
                    self._stop_owned_process()
                    raise NeuralTtsError(abort_reason)
                message = self._receive()
                if message is None:
                    continue
                if message.get('task_id') != task_id or message.get('type') not in ('audio', 'cancelled', 'error'):
                    raise NeuralTtsError('neural_tts_unexpected_message')
                # 取消完成通知在子进程的 CUDA 工作结束后发送，晚到的音频也直接丢弃
                if abort_reason or self.closed.is_set() or cancel.is_set():
                    raise NeuralTtsError(abort_reason or 'cancelled')
                if message['type'] != 'audio':
                    raise NeuralTtsError(message.get('code', 'neural_tts_generation_failed'))
                if (message.get('sample_rate') != 24000 or message.get('voice_name') != self.speakers[voice]
                        or message.get('voice_identity') != self.identities[voice]):
                    raise NeuralTtsError('neural_tts_invalid_audio_format')
                pcm = base64.b64decode(message.get('pcm', ''), validate=True)
                if not pcm or len(pcm) > 384000 or len(pcm) % 2:
                    raise NeuralTtsError('neural_tts_invalid_pcm')
                self._completed_voice.name = message['voice_name']
                return pcm, 24000
        except BaseException as error:
            # 已收到明确终态时保留常驻模型；管道或协议异常时停止自身子进程
            terminal_errors = ('cancelled', 'neural_tts_timeout', 'neural_tts_audio_too_long',
                               'neural_tts_generation_failed', 'neural_tts_token_limit')
            if not isinstance(error, NeuralTtsError) or error.code not in terminal_errors:
                self._stop_owned_process()
            raise
        finally:
            self.lock.release()

    def close(self):
        self.closed.set()
        wait = self.config['cancel_grace_seconds'] + self.config['process_exit_timeout_seconds'] + 1
        if not self.lock.acquire(timeout=wait):
            raise NeuralTtsError('neural_tts_close_pending')
        try:
            if self.process and self.process.poll() is None:
                try:
                    self._send({'type': 'close'})
                    self.process.wait(timeout=self.config['cancel_grace_seconds'])
                except (OSError, subprocess.TimeoutExpired):
                    pass
            self._stop_owned_process()
        finally:
            self.lock.release()
