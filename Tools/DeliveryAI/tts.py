"""逐句合成语音，本机默认使用已安装的 Windows 声音，可配置独立 TTS API"""
import base64
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import time
import urllib.request


def local_shell():
    override = os.environ.get('DELIVERY_TTS_SHELL')
    if override:
        executable = shutil.which(override)
        if not executable:
            raise RuntimeError('tts_shell_not_found')
        return executable
    executable = shutil.which('pwsh.exe') or shutil.which('powershell.exe')
    if not executable:
        raise RuntimeError('tts_shell_not_found')
    return executable


class SpeechSynthesizer:
    def __init__(self):
        self.process = None
        self.lines = queue.Queue()
        self.lock = threading.Lock()
        self.closed = threading.Event()
        self.shell_path = ''
        self.available_voices = []
        self.voice_name = ''

    def close(self):
        self.closed.set()
        with self.lock:
            self._stop_process()

    def _stop_process(self):
        if self.process:
            self.process.kill()
            self.process.wait(timeout=3)
            self.process = None

    def prepare(self, cancel):
        if os.environ.get('DELIVERY_TTS_URL'):
            return False
        with self.lock:
            self._start_local(cancel)
        return True

    def _start_local(self, cancel):
        if self.closed.is_set() or cancel.is_set():
            raise RuntimeError('cancelled')
        if self.process and self.process.poll() is None:
            return
        self.lines = queue.Queue()
        # 优先使用已安装的 PowerShell 7，读取系统提供的更多人物声音
        self.shell_path = local_shell()
        self.process = subprocess.Popen(
            [self.shell_path, '-NoProfile', '-NonInteractive', '-File', str(Path(__file__).with_name('tts_worker.ps1'))],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding='utf-8', creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        process, output = self.process, self.lines
        def read_lines():
            for line in process.stdout:
                output.put(line)
        threading.Thread(target=read_lines, daemon=True).start()
        # 语音服务完成初始化后通知 ready，第一句台词直接开始合成
        deadline = time.monotonic()+8
        try:
            while True:
                if self.closed.is_set() or cancel.is_set() or time.monotonic() >= deadline or self.process.poll() is not None:
                    raise RuntimeError('speech_prepare_cancelled_or_timeout')
                try:
                    result = json.loads(self.lines.get(timeout=.05))
                    if result.get('ready') is not True:
                        raise ValueError('speech_prepare_failed')
                    self.available_voices = result.get('voices', [])
                    if not isinstance(self.available_voices, list) or any(not isinstance(voice, str) for voice in self.available_voices):
                        raise ValueError('invalid_voice_list')
                    return
                except queue.Empty:
                    continue
        except Exception:
            self._stop_process()
            raise

    def synthesize(self, text, voice, emotion, cancel):
        if not text or len(text) > 120 or voice not in ('male', 'female'):
            raise ValueError('invalid_speech')
        if self.closed.is_set() or cancel.is_set():
            raise RuntimeError('cancelled')
        if os.environ.get('DELIVERY_TTS_URL'):
            return self._api(text, voice, emotion, cancel)
        with self.lock:
            self._start_local(cancel)
            self.process.stdin.write(json.dumps({'text': text, 'voice': voice, 'emotion': emotion}, ensure_ascii=False)+'\n')
            self.process.stdin.flush()
            deadline = time.monotonic()+8
            while True:
                if self.closed.is_set() or cancel.is_set() or time.monotonic() >= deadline or self.process.poll() is not None:
                    self._stop_process()
                    raise RuntimeError('speech_cancelled_or_timeout')
                try:
                    result = json.loads(self.lines.get(timeout=.05))
                    pcm = base64.b64decode(result['pcm'], validate=True)
                    if not pcm or len(pcm) > 384000 or len(pcm) % 2:
                        raise ValueError('invalid_pcm')
                    self.voice_name = result.get('voice_name', '')
                    if not isinstance(self.voice_name, str):
                        raise ValueError('invalid_voice_name')
                    return pcm, 16000
                except queue.Empty:
                    continue

    def _api(self, text, voice, emotion, cancel):
        payload = {'model': os.environ.get('DELIVERY_TTS_MODEL', 'gpt-4o-mini-tts'),
                   'voice': os.environ.get('DELIVERY_TTS_VOICE_'+voice.upper(), 'onyx' if voice == 'male' else 'coral'),
                   'input': text, 'response_format': 'pcm'}
        headers = {'Content-Type': 'application/json'}
        if os.environ.get('DELIVERY_TTS_API_KEY'):
            headers['Authorization'] = 'Bearer '+os.environ['DELIVERY_TTS_API_KEY']
        request = urllib.request.Request(os.environ['DELIVERY_TTS_URL'], json.dumps(payload).encode(), headers)
        with urllib.request.urlopen(request, timeout=8) as response:
            pcm = bytearray()
            while not cancel.is_set():
                part = response.read(8192)
                if not part:
                    break
                pcm.extend(part)
                if len(pcm) > 384000:
                    raise ValueError('speech_too_large')
        if cancel.is_set() or not pcm or len(pcm) % 2:
            raise ValueError('invalid_speech')
        self.voice_name = payload['voice']
        return bytes(pcm), 24000
