"""玩家语音优先开始；已经开始的合成仍由原调用负责结束"""
from concurrent.futures import Future
from dataclasses import dataclass
import heapq
import math
import threading
import time


@dataclass
class _Work:
    function: object
    args: tuple
    future: Future
    cancel: threading.Event


class _PlayerReservation:
    """玩家仍在生成台词时保留优先权，释放可以重复调用"""

    def __init__(self, owner, key):
        self._owner = owner
        self._key = key

    def release(self):
        self._owner._release_player_reservation(self._key)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.release()


class SpeechQueue:
    """后端持有的语音队列，每个实例独立管理线程、等待任务和取消通知"""

    def __init__(self, workers=1, capacity=24):
        if type(workers) is not int or workers < 1 or type(capacity) is not int or capacity < 1:
            raise ValueError('workers and capacity must be positive integers')
        self._capacity = capacity
        self._condition = threading.Condition()
        self._stop = threading.Event()
        self._waiting = []
        self._active = {}
        self._player_reservations = {}
        self._next_id = 0
        self._threads = [threading.Thread(target=self._worker, name=f'npc-speech-{index}', daemon=True)
                         for index in range(workers)]
        # 合成线程可能都在等待服务返回，另一个线程负责及时移除已取消的排队任务
        self._threads.append(threading.Thread(target=self._watch_cancellations,
                                              name='npc-speech-cancel', daemon=True))
        for thread in self._threads:
            thread.start()

    def reserve_player(self, cancel=None):
        """玩家生成回复期间暂停新的闲聊合成，玩家语音和已运行任务继续处理

        调用方在模型结束或异常时释放；取消通知也会解除等待，不占用语音队列容量
        """
        if cancel is not None and not isinstance(cancel, threading.Event):
            raise TypeError('cancel must be a threading.Event')
        cancel = cancel if cancel is not None else threading.Event()
        key = object()
        with self._condition:
            if self._stop.is_set():
                raise RuntimeError('speech_queue_closed')
            if not cancel.is_set():
                self._player_reservations[key] = cancel
            self._condition.notify_all()
        return _PlayerReservation(self, key)

    def _release_player_reservation(self, key):
        with self._condition:
            self._player_reservations.pop(key, None)
            self._condition.notify_all()

    def _remove_cancelled_reservations_locked(self):
        cancelled = [key for key, cancel in self._player_reservations.items() if cancel.is_set()]
        for key in cancelled:
            del self._player_reservations[key]
        if cancelled:
            self._condition.notify_all()

    def submit(self, function, *args, priority=0, cancel=None):
        """提交一次调用，0 为玩家、1 为闲聊，同类按提交顺序开始

        cancel 只用于队列检查与关闭通知，调用方需把同一 Event 传给实际合成函数
        每次提交返回独立 Future，运行中的取消只有在函数真正返回或抛出异常后才结束
        """
        if not callable(function):
            raise TypeError('function must be callable')
        if type(priority) is not int or priority not in (0, 1):
            raise ValueError('priority must be 0 or 1')
        if cancel is not None and not isinstance(cancel, threading.Event):
            raise TypeError('cancel must be a threading.Event')
        cancel = cancel if cancel is not None else threading.Event()
        future = Future()
        full = False
        with self._condition:
            if self._stop.is_set():
                raise RuntimeError('speech_queue_closed')
            removed = self._remove_cancelled_locked()
            # 已取消的新请求直接返回取消结果，不占队列容量
            if cancel.is_set():
                removed.append(_Work(function, args, future, cancel))
            elif len(self._waiting)+len(self._active) >= self._capacity:
                full = True
            else:
                sequence = self._next_id
                self._next_id += 1
                heapq.heappush(self._waiting, (priority, sequence, _Work(function, args, future, cancel)))
                future.add_done_callback(self._wake_on_cancel)
            self._condition.notify_all()
        # Future 回调可能调用队列，执行回调时不持有队列的锁
        self._cancel_waiting(removed)
        if full:
            raise RuntimeError('speech_queue_full')
        return future

    def _wake_on_cancel(self, future):
        if future.cancelled():
            with self._condition:
                self._condition.notify_all()

    def _remove_cancelled_locked(self):
        removed, remaining = [], []
        for item in self._waiting:
            work = item[2]
            if work.cancel.is_set() or work.future.cancelled():
                removed.append(work)
            else:
                remaining.append(item)
        if removed:
            self._waiting = remaining
            heapq.heapify(self._waiting)
            self._condition.notify_all()
        return removed

    @staticmethod
    def _cancel_waiting(tasks):
        for work in tasks:
            work.future.cancel()

    def _watch_cancellations(self):
        while True:
            with self._condition:
                removed = self._remove_cancelled_locked()
                self._remove_cancelled_reservations_locked()
                if not removed:
                    if self._stop.is_set():
                        return
                    self._condition.wait(timeout=.05 if self._waiting or self._player_reservations else None)
                    continue
            self._cancel_waiting(removed)

    def _worker(self):
        while True:
            work = None
            with self._condition:
                while True:
                    removed = self._remove_cancelled_locked()
                    self._remove_cancelled_reservations_locked()
                    if removed:
                        break
                    # 玩家台词还没生成时，闲聊不能抢先开始；已排队的玩家语音仍可执行
                    if self._waiting and (not self._player_reservations or self._waiting[0][0] == 0):
                        _, sequence, candidate = heapq.heappop(self._waiting)
                        if not candidate.future.set_running_or_notify_cancel():
                            self._condition.notify_all()
                            continue
                        work = candidate
                        self._active[sequence] = work
                        break
                    if self._stop.is_set():
                        return
                    self._condition.wait()
            self._cancel_waiting(removed)
            if work is None:
                continue
            try:
                result = work.function(*work.args)
            except BaseException as error:
                work.future.set_exception(error)
            else:
                work.future.set_result(result)
            finally:
                with self._condition:
                    del self._active[sequence]
                    self._condition.notify_all()

    def close(self, timeout=0):
        """停止接收请求并通知取消，返回 True 表示所有队列线程已经退出

        正在运行的函数可能还在等待服务返回，此时返回 False，之后可再次调用 close 等待
        """
        if not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout < 0:
            raise ValueError('timeout must be finite and nonnegative')
        deadline = time.monotonic()+timeout
        with self._condition:
            self._stop.set()
            waiting = [item[2] for item in self._waiting]
            self._waiting.clear()
            for cancel in self._player_reservations.values():
                cancel.set()
            self._player_reservations.clear()
            for work in waiting+list(self._active.values()):
                work.cancel.set()
            self._condition.notify_all()
        self._cancel_waiting(waiting)
        for thread in self._threads:
            if thread is threading.current_thread():
                continue
            remaining = max(0.0, deadline-time.monotonic())
            thread.join(timeout=min(remaining, threading.TIMEOUT_MAX))
        return all(not thread.is_alive() for thread in self._threads)
