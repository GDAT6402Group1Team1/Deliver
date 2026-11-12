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
        self.created = time.monotonic()
        self.finished = False
        # 模型调用结束后，由这个 Future 通知本轮语音任务已结束；不表示游戏已播放
        self.completion = None

    def emit(self, kind, **data):
        if self.cancel.is_set():
            return
        self.events.put({'type': kind, 'request_id': self.key[2], **data}, timeout=2)


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
        job.cancel.set()
        job.finished = True
        self.waiting.remove(job)
        self.jobs.pop(job.key, None)

    def cancel(self, key):
        with self.condition:
            job = self.jobs.get(tuple(key))
            if job:
                job.cancel.set()
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
                if not job.cancel.is_set():
                    try:
                        job.emit('error', reason=type(error).__name__)
                    except queue.Full:
                        pass
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
                job.cancel.set()
            self.condition.notify_all()
        for thread in self.threads:
            thread.join(timeout=8)
        with self.condition:
            # 语音收到取消后仍需退出实际合成，期间保持 Job 的未完成状态
            self.condition.wait_for(lambda: not self.jobs, timeout=9)
