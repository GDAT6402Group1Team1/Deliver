"""用可控的阻塞调用检查优先顺序、取消和关闭，不调用模型或语音服务"""
from concurrent.futures import CancelledError
import threading
import time
import unittest

from speech_queue import SpeechQueue


class SpeechQueueTests(unittest.TestCase):
    def setUp(self):
        self.queues = []
        self.gates = []

    def tearDown(self):
        for gate in self.gates:
            gate.set()
        for queue in self.queues:
            self.assertTrue(queue.close(timeout=1), '测试结束后仍有语音线程运行')

    def queue(self, **kwargs):
        queue = SpeechQueue(**kwargs)
        self.queues.append(queue)
        return queue

    def blocked(self, queue, cancel=None, priority=0, result='finished'):
        entered, gate = threading.Event(), threading.Event()
        self.gates.append(gate)

        def run():
            entered.set()
            if not gate.wait(3):
                raise TimeoutError('test gate was not released')
            return result

        future = queue.submit(run, priority=priority, cancel=cancel)
        self.assertTrue(entered.wait(1), '阻塞任务没有开始')
        return future, gate

    def test_player_priority_and_fifo_do_not_interrupt_running_ambient(self):
        queue = self.queue()
        running, gate = self.blocked(queue, priority=1)
        order = []
        futures = [queue.submit(lambda label=label: order.append(label) or label, priority=priority)
                   for label, priority in [('ambient-a', 1), ('player-a', 0), ('ambient-b', 1), ('player-b', 0)]]
        self.assertFalse(running.done())
        self.assertEqual(order, [])
        gate.set()
        self.assertEqual(running.result(timeout=1), 'finished')
        self.assertEqual([future.result(timeout=1) for future in futures],
                         ['ambient-a', 'player-a', 'ambient-b', 'player-b'])
        self.assertEqual(order, ['player-a', 'player-b', 'ambient-a', 'ambient-b'])

    def test_cancelled_waiting_task_frees_capacity_while_worker_is_blocked(self):
        queue = self.queue(capacity=2)
        _, gate = self.blocked(queue)
        cancel, cancelled = threading.Event(), threading.Event()
        called = []
        waiting = queue.submit(lambda: called.append('cancelled'), cancel=cancel)
        waiting.add_done_callback(lambda _: cancelled.set())
        with self.assertRaisesRegex(RuntimeError, 'full'):
            queue.submit(lambda: None)
        cancel.set()
        self.assertTrue(cancelled.wait(.3), '全部合成线程阻塞时仍应检查等待任务的取消')
        self.assertTrue(waiting.cancelled())
        replacement = queue.submit(lambda: 'replacement')
        gate.set()
        self.assertEqual(replacement.result(timeout=1), 'replacement')
        self.assertEqual(called, [])

    def test_future_cancel_also_removes_waiting_work(self):
        queue = self.queue(capacity=2)
        _, gate = self.blocked(queue)
        waiting = queue.submit(lambda: self.fail('取消的函数不能运行'))
        self.assertTrue(waiting.cancel())
        replacement = queue.submit(lambda: 7)
        gate.set()
        self.assertEqual(replacement.result(timeout=1), 7)
        with self.assertRaises(CancelledError):
            waiting.result()

    def test_running_cancellation_keeps_real_result_and_capacity(self):
        queue = self.queue(capacity=1)
        cancel = threading.Event()
        future, gate = self.blocked(queue, cancel=cancel, result=42)
        cancel.set()
        self.assertTrue(future.running())
        self.assertFalse(future.cancel())
        self.assertFalse(future.done())
        with self.assertRaisesRegex(RuntimeError, 'full'):
            queue.submit(lambda: None)
        gate.set()
        self.assertEqual(future.result(timeout=1), 42)
        self.assertFalse(future.cancelled())

    def test_close_notifies_active_cancels_waiting_and_can_wait_again(self):
        queue = self.queue()
        cancel = threading.Event()
        active, gate = self.blocked(queue, cancel=cancel)
        pending = queue.submit(lambda: self.fail('关闭后排队任务不能运行'))
        self.assertFalse(queue.close())
        self.assertTrue(cancel.is_set())
        self.assertTrue(pending.cancelled())
        self.assertFalse(active.done())
        with self.assertRaisesRegex(RuntimeError, 'closed'):
            queue.submit(lambda: None)
        started = time.monotonic()
        self.assertFalse(queue.close(timeout=.025))
        self.assertLess(time.monotonic()-started, .3)
        self.assertFalse(active.done())
        gate.set()
        self.assertTrue(queue.close(timeout=1))
        self.assertEqual(active.result(), 'finished')
        self.assertTrue(queue.close())

    def test_exception_finishes_one_future_and_worker_keeps_running(self):
        queue = self.queue()

        def fail():
            raise ValueError('synthesis failed')

        failed = queue.submit(fail)
        success = queue.submit(lambda: 'next')
        with self.assertRaisesRegex(ValueError, 'synthesis failed'):
            failed.result(timeout=1)
        self.assertEqual(success.result(timeout=1), 'next')

    def test_repeated_submissions_have_independent_futures(self):
        queue = self.queue()
        _, gate = self.blocked(queue)
        calls = []

        def run(value):
            calls.append(value)
            return value

        first = queue.submit(run, 'same')
        second = queue.submit(run, 'same')
        self.assertIsNot(first, second)
        self.assertTrue(first.cancel())
        gate.set()
        self.assertEqual(second.result(timeout=1), 'same')
        self.assertEqual(calls, ['same'])

    def test_multiple_workers_and_capacity_count_all_active_calls(self):
        queue = self.queue(workers=2, capacity=2)
        first, first_gate = self.blocked(queue)
        second, second_gate = self.blocked(queue)
        self.assertTrue(first.running() and second.running())
        with self.assertRaisesRegex(RuntimeError, 'full'):
            queue.submit(lambda: None)
        self.assertFalse(queue.close())
        first_gate.set();second_gate.set()
        self.assertTrue(queue.close(timeout=1))
        self.assertTrue(first.done() and second.done())

    def test_already_cancelled_submission_never_runs_or_uses_capacity(self):
        queue = self.queue(capacity=1)
        _, gate = self.blocked(queue)
        cancel = threading.Event();cancel.set()
        future = queue.submit(lambda: self.fail('已经取消的任务不能运行'), cancel=cancel)
        self.assertTrue(future.cancelled())
        gate.set()

    def test_instances_do_not_share_close_or_pending_work(self):
        first, second = self.queue(), self.queue()
        active, gate = self.blocked(first)
        self.assertFalse(first.close())
        self.assertEqual(second.submit(lambda: 'other queue').result(timeout=1), 'other queue')
        gate.set()
        self.assertEqual(active.result(timeout=1), 'finished')

    def test_invalid_options_are_rejected(self):
        for kwargs in ({'workers': 0}, {'workers': 1.5}, {'capacity': 0}, {'capacity': True}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                SpeechQueue(**kwargs)
        queue = self.queue()
        for priority in (-1, 2, True):
            with self.subTest(priority=priority), self.assertRaises(ValueError):
                queue.submit(lambda: None, priority=priority)
        for timeout in (-1, float('inf'), float('nan')):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                queue.close(timeout)
        self.assertEqual(queue.submit(lambda: 'still open').result(timeout=1), 'still open')


if __name__ == '__main__':
    unittest.main()
