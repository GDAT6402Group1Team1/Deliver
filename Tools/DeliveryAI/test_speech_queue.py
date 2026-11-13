"""用可控的阻塞调用检查优先顺序、取消和关闭，不调用模型或语音服务"""
from concurrent.futures import CancelledError
import json
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

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

    def test_player_generation_holds_ambient_but_allows_ready_player_audio(self):
        queue = self.queue()
        reservation = queue.reserve_player()
        ambient_started = threading.Event()
        ambient = queue.submit(lambda: ambient_started.set() or 'ambient', priority=1)
        self.assertFalse(ambient_started.wait(.04), '玩家仍在生成台词时闲聊不能抢先合成')
        self.assertEqual(queue.submit(lambda: 'player', priority=0).result(timeout=1), 'player')
        self.assertFalse(ambient.done())
        reservation.release()
        reservation.release()
        self.assertEqual(ambient.result(timeout=1), 'ambient')

    def test_reservation_does_not_cancel_running_speech_or_reorder_player_fifo(self):
        queue = self.queue()
        cancel = threading.Event()
        active, gate = self.blocked(queue, priority=1, cancel=cancel)
        reservation = queue.reserve_player()
        order = []
        ambient = queue.submit(lambda: order.append('ambient'), priority=1)
        first = queue.submit(lambda: order.append('player-a'), priority=0)
        second = queue.submit(lambda: order.append('player-b'), priority=0)
        self.assertFalse(cancel.is_set())
        self.assertFalse(active.done())
        gate.set()
        active.result(timeout=1)
        first.result(timeout=1)
        second.result(timeout=1)
        self.assertEqual(order, ['player-a', 'player-b'])
        reservation.release()
        ambient.result(timeout=1)
        self.assertEqual(order, ['player-a', 'player-b', 'ambient'])

    def test_each_player_releases_only_its_own_reservation(self):
        queue = self.queue(workers=2)
        first = queue.reserve_player()
        second = queue.reserve_player()
        ambient_started = threading.Event()
        ambient = queue.submit(ambient_started.set, priority=1)
        first.release()
        first.release()
        self.assertEqual(queue.submit(lambda: 'ready player').result(timeout=1), 'ready player')
        self.assertFalse(ambient_started.wait(.04))
        second.release()
        ambient.result(timeout=1)

    def test_cancellation_releases_reservation_even_if_model_has_not_returned(self):
        queue = self.queue()
        cancel = threading.Event()
        reservation = queue.reserve_player(cancel=cancel)
        ambient_started = threading.Event()
        ambient = queue.submit(ambient_started.set, priority=1)
        self.assertFalse(ambient_started.wait(.04))
        cancel.set()
        self.assertTrue(ambient_started.wait(.3))
        ambient.result(timeout=1)
        reservation.release()
        reservation.release()

    def test_exception_and_queue_close_release_reservations(self):
        queue = self.queue()
        with self.assertRaisesRegex(ValueError, 'model failed'):
            with queue.reserve_player():
                ambient = queue.submit(lambda: 'resumed', priority=1)
                raise ValueError('model failed')
        self.assertEqual(ambient.result(timeout=1), 'resumed')
        cancel = threading.Event()
        reservation = queue.reserve_player(cancel=cancel)
        pending = queue.submit(lambda: self.fail('关闭后不能开始闲聊'), priority=1)
        self.assertTrue(queue.close(timeout=1))
        self.assertTrue(cancel.is_set())
        self.assertTrue(pending.cancelled())
        reservation.release()
        reservation.release()
        self.assertTrue(queue.close())
        with self.assertRaisesRegex(RuntimeError, 'closed'):
            queue.reserve_player()

    def test_already_cancelled_reservation_and_other_queue_do_not_pause_work(self):
        first, second = self.queue(), self.queue()
        cancel = threading.Event()
        cancel.set()
        reservation = first.reserve_player(cancel=cancel)
        self.assertEqual(first.submit(lambda: 'first', priority=1).result(timeout=1), 'first')
        reservation.release()
        reservation = first.reserve_player()
        self.assertEqual(second.submit(lambda: 'second', priority=1).result(timeout=1), 'second')
        reservation.release()

    def test_run_job_releases_reservation_when_model_finishes_or_raises(self):
        from scheduler import Job
        from streaming import run_job
        for failure in (False, True):
            with self.subTest(failure=failure):
                queue = self.queue()
                entered, gate, ambient_started = threading.Event(), threading.Event(), threading.Event()
                self.gates.append(gate)
                job = Job(dict(world_epoch='world', npc_id='npc', request_id=str(failure),
                               request_source='player', persona='测试街坊', evidence=[]))
                errors = []

                def model(*_):
                    entered.set()
                    if not gate.wait(2):
                        raise TimeoutError('test model was not released')
                    if failure:
                        raise ValueError('model failed')
                    return {'speech': '', 'emotion': 'Calm'}

                def run():
                    try:
                        run_job(job, 'fixture', None, model)
                    except Exception as error:
                        errors.append(error)

                with patch('streaming.uses_neural_speech', return_value=True), \
                     patch('streaming.neural_speech_queue', return_value=queue), \
                     patch('streaming.model_context', return_value={'current_player_message': None, 'candidates': [], 'subjects': []}), \
                     patch('streaming.dialogue_log.record'), patch('builtins.print'):
                    worker = threading.Thread(target=run, daemon=True)
                    try:
                        worker.start()
                        self.assertTrue(entered.wait(1))
                        ambient = queue.submit(ambient_started.set, priority=1)
                        self.assertFalse(ambient_started.wait(.04))
                        gate.set()
                        worker.join(1)
                        self.assertFalse(worker.is_alive())
                        ambient.result(timeout=1)
                        self.assertTrue(job.completion.done())
                        self.assertEqual(len(errors), int(failure))
                        if failure:
                            self.assertIsInstance(errors[0], ValueError)
                    finally:
                        gate.set()
                        worker.join(2)

    def test_run_job_player_first_sentence_starts_before_generation_finishes(self):
        from scheduler import Job
        from server import validate_action
        from streaming import run_job
        from test_server import context
        speech_queue = self.queue()
        entered, allow_sentence, finish_model = threading.Event(), threading.Event(), threading.Event()
        voice_started, ambient_started = threading.Event(), threading.Event()
        self.gates.extend((allow_sentence, finish_model))
        data = context()
        data['request_source'] = 'player'
        data['candidates'] = [{'action': 'Wait', 'target_id': '', 'duration': 1, 'evidence_ids': []}]
        job, errors = Job(data), []

        def stream(_, cancel, on_delta):
            entered.set()
            if not allow_sentence.wait(2):
                raise TimeoutError('sentence was not released')
            on_delta(json.dumps({'candidate_index': 0, 'speech': '你刚才说谁？', 'emotion': 'Calm'}))
            if not finish_model.wait(2):
                raise TimeoutError('model was not released')

        def synthesize(*_):
            voice_started.set()
            return b'\x01\x00' * 16, 16000

        def run():
            try:
                run_job(job, 'llm', validate_action, None)
            except Exception as error:
                errors.append(error)

        with patch('streaming.uses_neural_speech', return_value=True), \
             patch('streaming.neural_speech_queue', return_value=speech_queue), \
             patch('streaming.neural_speech_synthesizer', return_value=SimpleNamespace(synthesize=synthesize)), \
             patch('streaming.stream_api', side_effect=stream), \
             patch('streaming.dialogue_log.record'), patch('builtins.print'):
            worker = threading.Thread(target=run, daemon=True)
            try:
                worker.start()
                self.assertTrue(entered.wait(1))
                ambient = speech_queue.submit(ambient_started.set, priority=1)
                self.assertFalse(ambient_started.wait(.04))
                allow_sentence.set()
                self.assertTrue(voice_started.wait(1), '玩家首句就绪后应该开始合成')
                self.assertFalse(ambient.done())
                self.assertFalse(job.completion.done(), '模型还在继续生成')
                finish_model.set()
                worker.join(1)
                self.assertFalse(worker.is_alive())
                job.completion.result(timeout=1)
                ambient.result(timeout=1)
                self.assertEqual(errors, [])
            finally:
                allow_sentence.set()
                finish_model.set()
                worker.join(2)

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
        with self.assertRaises(TypeError):
            queue.reserve_player(cancel=True)
        for timeout in (-1, float('inf'), float('nan')):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                queue.close(timeout)
        self.assertEqual(queue.submit(lambda: 'still open').result(timeout=1), 'still open')


if __name__ == '__main__':
    unittest.main()
