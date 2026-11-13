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
MAX_STREAM_CHUNKS = 32
# 一个任务最多 32 块，另留启动和结束通知；父进程一次只提交一句
MAX_WORKER_MESSAGES = MAX_STREAM_CHUNKS + 2


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
        if (set(female) not in (fields, fields | {'predictor_mode'}) or female['speaker'] != 'Xiaofang_B'
                or not isinstance(female['reference_text'], str)
                or not 1 <= len(female['reference_text'].strip()) <= 120):
            raise NeuralTtsError('neural_tts_invalid_voices')
        if female.get('predictor_mode', 'official') not in ('official', 'cuda_graph'):
            raise NeuralTtsError('neural_tts_invalid_predictor_mode')
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
            # 旧进程的女声只有 official；Graph 必须明确标记，不能作为原版声音接收
            if entry.get('predictor_mode', 'official') != 'official':
                identity['predictor_mode'] = entry['predictor_mode']
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
        self.audio_stream_version = 0
        self.closed = threading.Event()
        self.lock = threading.Lock()
        self.messages = queue.Queue(maxsize=MAX_WORKER_MESSAGES)
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
        self.audio_stream_version = 0
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
        self.messages = queue.Queue(maxsize=MAX_WORKER_MESSAGES)
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
                version = message.get('audio_stream_version', 0)
                self.audio_stream_version = version if type(version) is int and version == 1 else 0
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

    @property
    def supports_audio_stream(self):
        return self.ready and self.audio_stream_version == 1

    def synthesize(self, text, voice, emotion, cancel):
        return self._synthesize(text, voice, emotion, cancel)

    def synthesize_stream(self, text, voice, emotion, cancel, on_chunk, chunk_frames=8):
        """每段通过 on_chunk 返回 PCM，完成后返回累计字节数和采样率"""
        if not callable(on_chunk) or type(chunk_frames) is not int or chunk_frames not in (4, 8):
            raise NeuralTtsError('neural_tts_invalid_stream_options')
        return self._synthesize(text, voice, emotion, cancel, on_chunk, chunk_frames)

    def _synthesize(self, text, voice, emotion, cancel, on_chunk=None, chunk_frames=0):
        if (not isinstance(text, str) or not text.strip() or len(text) > 120
                or not isinstance(voice, str) or voice not in self.speakers):
            raise NeuralTtsError('neural_tts_invalid_speech')
        if emotion not in ('Calm', 'Angry', 'Surprised'):
            raise NeuralTtsError('neural_tts_invalid_emotion')
        self._acquire(cancel)
        terminal_received = False
        task_sent = False
        abort_error = None
        reuse_after_abort = False
        try:
            self._prepare_locked(cancel)
            if on_chunk is not None and not self.supports_audio_stream:
                raise NeuralTtsError('neural_tts_stream_unsupported')
            task_id = uuid.uuid4().hex
            command = {'type': 'synthesize', 'task_id': task_id, 'text': text, 'voice': voice, 'emotion': emotion}
            if on_chunk is not None:
                command['chunk_frames'] = chunk_frames
            self._send(command)
            task_sent = True
            deadline = time.monotonic() + self.config['synthesis_timeout_seconds']
            chunk_count = total_bytes = 0

            def abort(error, can_reuse=False):
                nonlocal abort_error, deadline, reuse_after_abort
                if abort_error is None:
                    abort_error = error
                    reuse_after_abort = can_reuse
                    self._send({'type': 'cancel', 'task_id': task_id})
                    deadline = time.monotonic() + self.config['cancel_grace_seconds']

            def check_identity(message):
                if (message.get('sample_rate') != 24000 or message.get('voice_name') != self.speakers[voice]
                        or message.get('voice_identity') != self.identities[voice]):
                    raise NeuralTtsError('neural_tts_invalid_audio_format')

            while True:
                if abort_error is None and (self.closed.is_set() or cancel.is_set() or time.monotonic() >= deadline):
                    reason = 'cancelled' if self.closed.is_set() or cancel.is_set() else 'neural_tts_timeout'
                    abort(NeuralTtsError(reason), can_reuse=True)
                if abort_error is not None and time.monotonic() >= deadline:
                    self._stop_owned_process()
                    raise abort_error
                message = self._receive()
                if message is None:
                    continue
                kind = message.get('type')
                accepted = ('audio_chunk', 'audio_end', 'cancelled', 'error') if on_chunk is not None else (
                    'audio', 'cancelled', 'error')
                if message.get('task_id') != task_id or kind not in accepted:
                    raise NeuralTtsError('neural_tts_unexpected_message')
                if abort_error is None and (self.closed.is_set() or cancel.is_set()):
                    abort(NeuralTtsError('cancelled'), can_reuse=True)
                # 迟到的音频块不能表示工作已经停止，继续收取直到子进程确认结束
                terminal_received = kind != 'audio_chunk'
                if abort_error is not None:
                    if terminal_received:
                        raise abort_error
                    continue
                if kind in ('cancelled', 'error'):
                    raise NeuralTtsError(message.get('code', 'neural_tts_generation_failed'))
                if kind == 'audio_chunk':
                    try:
                        check_identity(message)
                        if (type(message.get('chunk_index')) is not int or message['chunk_index'] != chunk_count
                                or type(message.get('offset_bytes')) is not int or message['offset_bytes'] != total_bytes
                                or chunk_count >= MAX_STREAM_CHUNKS):
                            raise NeuralTtsError('neural_tts_invalid_stream_order')
                        pcm = base64.b64decode(message.get('pcm', ''), validate=True)
                        if (not pcm or len(pcm) % 2 or len(pcm) > chunk_frames * 3840
                                or total_bytes + len(pcm) > self.config['max_pcm_bytes']):
                            raise NeuralTtsError('neural_tts_invalid_stream_size')
                    except Exception as error:
                        abort(error)
                        continue
                    try:
                        on_chunk(pcm)
                        chunk_count += 1
                        total_bytes += len(pcm)
                    except Exception as error:
                        # 回调可能因网络队列已满而失败，确认合成停止后仍可继续使用模型
                        abort(error, can_reuse=True)
                    continue
                check_identity(message)
                if kind == 'audio_end':
                    if (type(message.get('chunk_count')) is not int or message['chunk_count'] != chunk_count
                            or type(message.get('total_pcm_bytes')) is not int or message['total_pcm_bytes'] != total_bytes
                            or not chunk_count or not total_bytes):
                        raise NeuralTtsError('neural_tts_invalid_stream_end')
                    self._completed_voice.name = message['voice_name']
                    return total_bytes, 24000
                pcm = base64.b64decode(message.get('pcm', ''), validate=True)
                if not pcm or len(pcm) > 384000 or len(pcm) % 2:
                    raise NeuralTtsError('neural_tts_invalid_pcm')
                self._completed_voice.name = message['voice_name']
                return pcm, 24000
        except BaseException as error:
            # 已收到明确终态时保留常驻模型；管道或协议异常时停止自身子进程
            terminal_errors = ('cancelled', 'neural_tts_timeout', 'neural_tts_audio_too_long',
                               'neural_tts_generation_failed', 'neural_tts_token_limit')
            reusable = (not task_sent and isinstance(error, NeuralTtsError)
                        and error.code == 'neural_tts_stream_unsupported')
            if task_sent and terminal_received:
                reusable = (reuse_after_abort if abort_error is not None else
                            isinstance(error, NeuralTtsError) and error.code in terminal_errors)
            if not reusable:
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
