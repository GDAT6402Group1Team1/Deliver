"""按顺序发送语音块，让模型生成和管道写入各用一个线程"""
import base64
import queue
import threading
import time

from neural_tts import NeuralTtsError


class AudioChunkSender:
    """队列有上限；取消后丢弃未发送的块，退出前等待正在写出的块结束"""

    def __init__(self, callback, check, capacity=4):
        self.callback, self.check = callback, check
        self.pending = queue.Queue(maxsize=capacity)
        self.stopped = threading.Event()
        self.error = None
        self.sent_count = self.sent_bytes = 0
        self.queue_wait_ms = self.send_ms = 0.0
        self.thread = threading.Thread(target=self._run, name='qwen-audio-output', daemon=True)
        self.thread.start()

    def check_error(self):
        self.check()
        if self.error is not None:
            raise self.error

    def enqueue(self, message, pcm):
        started = time.monotonic()
        while True:
            self.check_error()
            if self.stopped.is_set():
                raise NeuralTtsError('neural_tts_stream_callback_failed')
            try:
                self.pending.put((message, pcm), timeout=.02)
                self.queue_wait_ms += (time.monotonic() - started) * 1000
                return
            except queue.Full:
                continue

    def _run(self):
        try:
            while not self.stopped.is_set():
                try:
                    entry = self.pending.get(timeout=.02)
                except queue.Empty:
                    continue
                if entry is None:
                    return
                self.check()
                if self.stopped.is_set():
                    return
                message, pcm = entry
                message['pcm'] = base64.b64encode(pcm).decode('ascii')
                self.check()
                started = time.monotonic()
                self.callback(message)
                self.send_ms += (time.monotonic() - started) * 1000
                self.sent_count += 1
                self.sent_bytes += len(pcm)
                self.check()
        except NeuralTtsError as error:
            self.error = error
        except Exception as error:
            self.error = NeuralTtsError('neural_tts_stream_callback_failed')
            self.error.__cause__ = error
        finally:
            self.stopped.set()

    def finish(self):
        # 结束消息排在最后一块之后，返回时所有声音都已写出
        while True:
            self.check_error()
            if self.stopped.is_set():
                raise NeuralTtsError('neural_tts_stream_callback_failed')
            try:
                self.pending.put(None, timeout=.02)
                break
            except queue.Full:
                continue
        while self.thread.is_alive():
            self.check_error()
            self.thread.join(.02)
        self.check_error()

    def close(self):
        self.stopped.set()
        # 正在写管道的回调必须退出，才能开始下一句；管道失联由父进程超时终止本进程
        self.thread.join()
        while True:
            try:
                self.pending.get_nowait()
            except queue.Empty:
                break
