"""检查满队列、玩家优先和取消，运行中的回调必须真正结束"""
import threading
import unittest

from scheduler import Scheduler


class ControlledWork:
    def __init__(self):
        self.condition = threading.Condition()
        self.order = []
        self.gates = {}

    def hold(self, name):
        self.gates[name] = threading.Event()

    def run(self, job):
        name = job.context['npc_id']
        with self.condition:
            self.order.append(name)
            self.condition.notify_all()
        # 故意等显式结束，检查 cancel 不会提前让另一项工作开始
        if name in self.gates and not self.gates[name].wait(2):
            raise TimeoutError('test did not release work')

    def started(self, name):
        with self.condition:
            return self.condition.wait_for(lambda: name in self.order, 1)

    def release(self):
        for gate in self.gates.values():
            gate.set()


def context(name, player=False):
    return dict(world_epoch='test-world', npc_id=name, request_id=name,
                request_source='player' if player else 'ambient')


class PlayerSchedulingTests(unittest.TestCase):
    def create(self, workers=2, capacity=8):
        work = ControlledWork()
        scheduler = Scheduler(work.run, workers, capacity)
        def clean():
            work.release()
            scheduler.close()
            self.assertFalse(any(thread.is_alive() for thread in scheduler.threads))
        self.addCleanup(clean)
        return work, scheduler

    def wait_finished(self, scheduler, job):
        with scheduler.condition:
            self.assertTrue(scheduler.condition.wait_for(lambda: job.finished, 1))

    def test_full_queue_replaces_only_waiting_ambient_and_preserves_player_order(self):
        work, scheduler = self.create(workers=1, capacity=3)
        work.hold('running')
        running = scheduler.submit(context('running'))
        self.assertTrue(work.started('running'))
        oldest = scheduler.submit(context('oldest'))
        newer = scheduler.submit(context('newer'))
        first = scheduler.submit(context('player-first', True))
        self.assertTrue(oldest.finished and oldest.cancel.is_set())
        self.assertFalse(running.finished or running.cancel.is_set())
        # 重复提交不能顺手取消另一项闲聊
        with self.assertRaisesRegex(RuntimeError, 'busy'):
            scheduler.submit(context('player-first', True))
        self.assertFalse(newer.cancel.is_set())
        second = scheduler.submit(context('player-second', True))
        self.assertTrue(newer.finished and newer.cancel.is_set())
        with self.assertRaisesRegex(RuntimeError, 'busy'):
            scheduler.submit(context('player-third', True))
        self.assertEqual(len(scheduler.jobs), 3)
        work.release()
        self.wait_finished(scheduler, second)
        self.assertTrue(first.finished)
        self.assertEqual(work.order, ['running', 'player-first', 'player-second'])

    def test_running_players_pause_new_ambient_without_blocking_another_player(self):
        work, scheduler = self.create()
        work.hold('player-first')
        work.hold('player-second')
        first = scheduler.submit(context('player-first', True))
        self.assertTrue(work.started('player-first'))
        ambient = scheduler.submit(context('ambient'))
        second = scheduler.submit(context('player-second', True))
        self.assertTrue(work.started('player-second'))
        self.assertEqual(work.order, ['player-first', 'player-second'])
        work.gates['player-first'].set()
        self.wait_finished(scheduler, first)
        with scheduler.condition:
            self.assertIn(ambient, scheduler.waiting)
            self.assertEqual(scheduler.running_players, 1)
        work.gates['player-second'].set()
        self.wait_finished(scheduler, second)
        self.wait_finished(scheduler, ambient)
        self.assertEqual(work.order, ['player-first', 'player-second', 'ambient'])

    def test_cancel_waiting_removes_capacity_before_workers_finish(self):
        work, scheduler = self.create()
        work.hold('ambient-running')
        work.hold('player-running')
        scheduler.submit(context('ambient-running'))
        self.assertTrue(work.started('ambient-running'))
        scheduler.submit(context('player-running', True))
        self.assertTrue(work.started('player-running'))
        waiting = [scheduler.submit(context(f'waiting-{i}', True)) for i in range(6)]
        removed = waiting[0]
        self.assertTrue(scheduler.cancel(removed.key))
        self.assertTrue(removed.finished)
        self.assertNotIn(removed.key, scheduler.jobs)
        self.assertNotIn(removed, scheduler.waiting)
        self.assertFalse(scheduler.cancel(removed.key))
        replacement = scheduler.submit(context('replacement', True))
        self.assertEqual(len(scheduler.jobs), 8)
        work.release()
        self.wait_finished(scheduler, replacement)
        self.assertNotIn('waiting-0', work.order)

    def test_running_cancel_keeps_capacity_until_callback_returns(self):
        work, scheduler = self.create(workers=1, capacity=1)
        work.hold('running')
        running = scheduler.submit(context('running', True))
        self.assertTrue(work.started('running'))
        self.assertTrue(scheduler.cancel(running.key))
        self.assertFalse(running.finished)
        self.assertIn(running.key, scheduler.jobs)
        with self.assertRaisesRegex(RuntimeError, 'busy'):
            scheduler.submit(context('next', True))
        work.release()
        self.wait_finished(scheduler, running)
        following = scheduler.submit(context('next', True))
        self.wait_finished(scheduler, following)
        scheduler.close()
        scheduler.close()
        self.assertEqual(scheduler.running_players, 0)
        self.assertEqual(scheduler.running_ambient, 0)
        self.assertFalse(scheduler.jobs or scheduler.waiting)


if __name__ == '__main__':
    unittest.main()
