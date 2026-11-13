"""优先处理玩家消息，运行中的工作结束后再接下一项"""
import queue
import threading
import time


class Job:
    def __init__(self, context):
        self.context = context
        self.key = tuple(context[k] for k in ('world_epoch', 'npc_id', 'request_id'))
        self.priority = 0 if context.get('request_source') == 'player' else 1
        self.cancel = threading.Event()
        self.events = queue.Queue(maxsize=64)
        self._terminal_lock = threading.Lock()
        self._failure = None
        self._cancelled_by_request = False
        self.created = time.monotonic()
        self.finished = False
        # 模型调用结束后，由这个 Future 通知本轮语音任务已结束；不表示游戏已播放
        self.completion = None

    def emit(self, kind, **data):
        if kind == 'error':
            self.fail(data.get('reason', 'reply_failed'))
            return
        event = {'type': kind, 'request_id': self.key[2], **data}
        deadline = time.monotonic() + 2
        while True:
            # 和取消共用同一把锁，避免排队等待结束后又塞入已取消的声音
            with self._terminal_lock:
                if self.cancel.is_set():
                    return
                try:
                    self.events.put_nowait(event)
                    return
                except queue.Full:
                    pass
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.fail('reply_queue_full')
                raise queue.Full('reply_queue_full')
            self.cancel.wait(min(.05, remaining))

    def fail(self, reason):
        """内部失败停止剩余工作，同时保留一次发给游戏的错误通知"""
        with self._terminal_lock:
            if self.cancel.is_set():
                return
            self._failure = {'type':'error', 'request_id':self.key[2], 'reason':reason}
            try:
                self.events.put_nowait(self._failure)
            except queue.Full:
                # 网络线程下一次发送前仍会读取 _failure，队列里的旧台词不会继续发送
                pass
            self.cancel.set()

    def cancel_request(self):
        """玩家打断或连接断开后，停止工作并丢弃尚未发送的全部内容"""
        with self._terminal_lock:
            self._cancelled_by_request = True
            self._failure = None
            self.cancel.set()

    def event_for_client(self, event):
        """发送前检查本轮状态，失败通知替代旧台词，真正取消后不再发送"""
        with self._terminal_lock:
            if self._cancelled_by_request:
                return None
            if self._failure is not None:
                return self._failure
            return None if self.cancel.is_set() else event


class Scheduler:
    def __init__(self, run, workers=2, capacity=8):
        self.run = run
        self.capacity = capacity
        self.condition = threading.Condition()
        self.waiting = []
        self.jobs = {}
        self.closed = False
        self.running_ambient = 0
        self.running_players = 0
        self.threads = [threading.Thread(target=self._worker, daemon=True) for _ in range(workers)]
        for thread in self.threads:
            thread.start()

    def submit(self, context):
        job = Job(context)
        with self.condition:
            if self.closed or job.key in self.jobs:
                raise RuntimeError('busy')
            if len(self.jobs) >= self.capacity:
                # 玩家可以替换最早排队的闲聊，已经开始的工作继续处理
                ambient = next((item for item in self.waiting if item.priority == 1), None)
                if job.priority != 0 or ambient is None:
                    raise RuntimeError('busy')
                self._remove_waiting(ambient)
            self.jobs[job.key] = job
            self.waiting.append(job)
            self.waiting.sort(key=lambda item: (item.priority, item.created))
            self.condition.notify_all()
        return job

    def _remove_waiting(self, job):
        # 调用时已持有 condition；没有开始的请求可以立即结束
        job.cancel_request()
        job.finished = True
        self.waiting.remove(job)
        self.jobs.pop(job.key, None)

    def cancel(self, key):
        with self.condition:
            job = self.jobs.get(tuple(key))
            if job:
                job.cancel_request()
                if job in self.waiting:
                    self._remove_waiting(job)
            self.condition.notify_all()
            return job is not None

    def _worker(self):
        while True:
            with self.condition:
                # 玩家交谈期间暂停新的闲聊；已开始的闲聊按原流程结束
                self.condition.wait_for(lambda: self.closed or any(
                    job.priority == 0 or job.cancel.is_set()
                    or (self.running_ambient == 0 and self.running_players == 0)
                    for job in self.waiting))
                if self.closed and not self.waiting:
                    return
                index = next((i for i, item in enumerate(self.waiting)
                              if item.priority == 0 or item.cancel.is_set()
                              or (self.running_ambient == 0 and self.running_players == 0)), 0)
                job = self.waiting.pop(index)
                if job.priority == 1:
                    self.running_ambient += 1
                else:
                    self.running_players += 1
            try:
                if not job.cancel.is_set():
                    print(f"latency request={job.key[2]} stage=worker_started scheduler_queue_ms={int((time.monotonic()-job.created)*1000)}", flush=True)
                    self.run(job)
            except Exception as error:
                job.fail(type(error).__name__)
            finally:
                # 模型函数已返回，工作线程可以接下一项；原回复等语音真正结束后再移除
                if job.completion is None:
                    self._finish(job)
                else:
                    job.completion.add_done_callback(lambda _, current=job: self._finish(current))

    def _finish(self, job):
        with self.condition:
            if self.jobs.get(job.key) is not job:
                return
            job.finished = True
            self.jobs.pop(job.key)
            if job.priority == 1:
                self.running_ambient -= 1
            else:
                self.running_players -= 1
            self.condition.notify_all()

    def close(self):
        with self.condition:
            self.closed = True
            for job in self.jobs.values():
                job.cancel_request()
            self.condition.notify_all()
        for thread in self.threads:
            thread.join(timeout=8)
        with self.condition:
            # 语音收到取消后仍需退出实际合成，期间保持 Job 的未完成状态
            self.condition.wait_for(lambda: not self.jobs, timeout=9)
