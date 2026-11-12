"""比较排队规则；模型和语音耗时由测试控制，不访问真实模型或正在运行的后端"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import queue
import statistics
import threading
import time
from pathlib import Path

from scheduler import Job, Scheduler


class ComparisonScheduler(Scheduler):
    """仅用于对照，生产基线直接使用 Scheduler，不修改其方法"""

    def __init__(self, run, policy, **kwargs):
        self.policy = policy
        self.running_players = 0
        super().__init__(run, **kwargs)

    def _remove_waiting(self, job):
        job.cancel.set()
        job.finished = True
        self.waiting.remove(job)
        self.jobs.pop(job.key)

    def submit(self, context):
        job = Job(context)
        with self.condition:
            if self.closed or job.key in self.jobs:
                raise RuntimeError('busy')
            if len(self.jobs) >= self.capacity:
                # 候选规则只替换未开始的闲聊，运行中的调用必须真正结束
                ambient = next((item for item in self.waiting if item.priority == 1), None)
                if self.policy != 'candidate' or job.priority != 0 or ambient is None:
                    raise RuntimeError('busy')
                self._remove_waiting(ambient)
            self.jobs[job.key] = job
            self.waiting.append(job)
            if self.policy == 'candidate':
                self.waiting.sort(key=lambda item: (item.priority, item.created))
            self.condition.notify_all()
        return job

    def cancel(self, key):
        if self.policy == 'fifo':
            # FIFO 对照保留旧取消方式，避免生产实现变化影响基线
            with self.condition:
                job = self.jobs.get(tuple(key))
                if job:
                    job.cancel.set()
                self.condition.notify_all()
                return job is not None
        with self.condition:
            job = self.jobs.get(tuple(key))
            if job:
                job.cancel.set()
                if job in self.waiting:
                    self._remove_waiting(job)
            self.condition.notify_all()
            return job is not None

    def _eligible(self, job):
        return (self.policy == 'fifo' or job.priority == 0 or job.cancel.is_set()
                or (self.running_ambient == 0 and self.running_players == 0))

    def _worker(self):
        while True:
            with self.condition:
                self.condition.wait_for(lambda: self.closed or any(self._eligible(j) for j in self.waiting))
                if self.closed and not self.waiting:
                    return
                index = next(i for i, job in enumerate(self.waiting) if self._eligible(job))
                job = self.waiting.pop(index)
                if job.priority == 1:
                    self.running_ambient += 1
                else:
                    self.running_players += 1
            try:
                if not job.cancel.is_set():
                    self.run(job)
            except Exception as error:
                if not job.cancel.is_set():
                    try:
                        job.emit('error', reason=type(error).__name__)
                    except queue.Full:
                        pass
            finally:
                with self.condition:
                    job.finished = True
                    self.jobs.pop(job.key, None)
                    if job.priority == 1:
                        self.running_ambient -= 1
                    else:
                        self.running_players -= 1
                    self.condition.notify_all()


class Workload:
    def __init__(self, model_ms):
        self.model_ms = model_ms
        self.condition = threading.Condition()
        self.started = {}
        self.model_finished = {}
        self.completed = {}
        self.gates = []

    def gate(self):
        gate = threading.Event()
        self.gates.append(gate)
        return gate

    def run(self, job):
        label = job.context['label']
        with self.condition:
            self.started[label] = time.monotonic()
            self.condition.notify_all()
        # 模拟有明确时长的调用；Event.wait 也让取消能够结束本段工作
        job.cancel.wait(self.model_ms / 1000)
        with self.condition:
            self.model_finished[label] = time.monotonic()
            self.condition.notify_all()
        gate = job.context.get('gate')
        if gate is not None and not gate.wait(3):
            raise TimeoutError('benchmark gate was not released')
        with self.condition:
            self.completed[label] = time.monotonic()
            self.condition.notify_all()

    def wait(self, field, label, timeout=2):
        with self.condition:
            return self.condition.wait_for(lambda: label in getattr(self, field), timeout)

    def release(self):
        for gate in self.gates:
            gate.set()


def make_context(label, source='ambient', gate=None):
    return dict(world_epoch='benchmark-world', npc_id=label, request_id=label,
                label=label, request_source=source, gate=gate)


def finish(scheduler, timeout=3):
    with scheduler.condition:
        if not scheduler.condition.wait_for(lambda: not scheduler.jobs, timeout):
            raise TimeoutError('benchmark jobs did not finish')


def run_case(policy, scenario, model_ms=15, probe_ms=75, baseline_factory=None):
    workload = Workload(model_ms)
    scheduler = (baseline_factory(workload.run, workers=2, capacity=8) if policy == 'baseline'
                 else Scheduler(workload.run, workers=2, capacity=8) if policy == 'current'
                 else ComparisonScheduler(workload.run, policy, workers=2, capacity=8))
    jobs, result = {}, {'policy': policy, 'scenario': scenario}

    def submit(label, source='ambient', gate=None):
        jobs[label] = scheduler.submit(make_context(label, source, gate))
        return jobs[label]

    def wait(field, label):
        if not workload.wait(field, label):
            raise TimeoutError(f'{label} never reached {field}')

    try:
        gate = workload.gate()
        if scenario == 'ambient_full':
            submit('a0', gate=gate)
            wait('started', 'a0')
            for number in range(1, 8):
                submit(f'a{number}', gate=gate)
            with scheduler.condition:
                result['jobs_before_player'] = len(scheduler.jobs)
                result['waiting_before_player'] = len(scheduler.waiting)
            try:
                submit('p0', 'player')
                result['player_accepted'] = True
                wait('started', 'p0')
            except RuntimeError:
                result['player_accepted'] = False
            result['ambient_cancelled'] = [label for label, job in jobs.items()
                                          if label.startswith('a') and job.cancel.is_set()]
        elif scenario == 'player_active':
            submit('p0', 'player', gate)
            wait('started', 'p0')
            submit('a0')
            result['ambient_started_while_player_active'] = workload.wait(
                'started', 'a0', probe_ms / 1000)
        elif scenario == 'cancel_waiting':
            submit('a0', gate=gate)
            wait('started', 'a0')
            submit('p0', 'player', gate)
            wait('started', 'p0')
            for number in range(1, 7):
                submit(f'a{number}')
            before = time.monotonic()
            scheduler.cancel(jobs['a1'].key)
            result['cancel_call_ms'] = (time.monotonic() - before) * 1000
            with scheduler.condition:
                result['cancel_removed_immediately'] = jobs['a1'].key not in scheduler.jobs
            try:
                submit('p1', 'player')
                result['replacement_player_accepted'] = True
            except RuntimeError:
                result['replacement_player_accepted'] = False
            with scheduler.condition:
                result['cancel_finished_before_worker_available'] = scheduler.condition.wait_for(
                    lambda: jobs['a1'].finished, probe_ms / 1000)
            if jobs['a1'].finished:
                result['cancel_finished_ms'] = (time.monotonic() - before) * 1000
            else:
                gate.set()
                with scheduler.condition:
                    if not scheduler.condition.wait_for(lambda: jobs['a1'].finished, 2):
                        raise TimeoutError('cancelled job remained queued')
                result['cancel_finished_ms'] = (time.monotonic() - before) * 1000
        elif scenario == 'tts_tail':
            # 对照生产 run_job 的边界：模型结束后，回调仍等待语音结果
            for label in ('p0', 'p1'):
                submit(label, 'player', gate)
                wait('model_finished', label)
            submit('p2', 'player')
            result['next_player_started_during_tail'] = workload.wait('started', 'p2', probe_ms / 1000)
            result['simulated_tail_hold_ms'] = probe_ms
        else:
            raise ValueError(scenario)
        workload.release()
        finish(scheduler)
        result['order'] = list(workload.started)
        result['queue_ms'] = {label: round((at - jobs[label].created) * 1000, 3)
                              for label, at in workload.started.items()}
        result['model_to_callback_end_ms'] = {
            label: round((workload.completed[label] - at) * 1000, 3)
            for label, at in workload.model_finished.items() if label in workload.completed}
        result['cancelled_job_started'] = 'a1' in workload.started if scenario == 'cancel_waiting' else None
        return result
    finally:
        workload.release()
        scheduler.close()
        if any(thread.is_alive() for thread in scheduler.threads):
            raise RuntimeError('benchmark worker did not stop')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--model-ms', type=float, default=15)
    parser.add_argument('--probe-ms', type=float, default=75)
    parser.add_argument('--output', default='scheduler-baseline.json')
    parser.add_argument('--baseline-snapshot', type=Path,
                        help='saved scheduler.py from before the change, for an unchanged comparison')
    args = parser.parse_args()
    if not 1 <= args.repeats <= 20 or not 1 <= args.model_ms <= 100 or not 10 <= args.probe_ms <= 300:
        parser.error('use repeats 1..20, model-ms 1..100 and probe-ms 10..300')
    root = Path(__file__).resolve().parents[2]
    out = root / 'Saved/AIResearch' / args.output
    if not out.resolve().is_relative_to((root / 'Saved/AIResearch').resolve()):
        parser.error('output must be inside Saved/AIResearch')
    baseline_factory = None
    policies = ('fifo', 'current', 'candidate')
    if args.baseline_snapshot:
        snapshot = args.baseline_snapshot.resolve()
        if not snapshot.is_relative_to((root / 'Saved/AIResearch').resolve()):
            parser.error('baseline snapshot must be inside Saved/AIResearch')
        spec = importlib.util.spec_from_file_location('saved_scheduler_baseline', snapshot)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        baseline_factory = module.Scheduler
        policies = ('fifo', 'baseline', 'current')
    trace = io.StringIO()
    rows = []
    with contextlib.redirect_stdout(trace):
        for repeat in range(args.repeats):
            for scenario in ('ambient_full', 'player_active', 'cancel_waiting', 'tts_tail'):
                for policy in policies:
                    rows.append(dict(repeat=repeat, **run_case(
                        policy, scenario, args.model_ms, args.probe_ms, baseline_factory)))
    summary = {}
    for policy in policies:
        selected = [row for row in rows if row['policy'] == policy]
        def cases(name):
            return [row for row in selected if row['scenario'] == name]
        summary[policy] = {
            'player_rejections_when_ambient_full': sum(not r['player_accepted'] for r in cases('ambient_full')),
            'ambient_starts_during_player': sum(r['ambient_started_while_player_active'] for r in cases('player_active')),
            'queued_cancel_median_ms': round(statistics.median(r['cancel_finished_ms'] for r in cases('cancel_waiting')), 3),
            'queued_cancel_immediate_count': sum(r['cancel_removed_immediately'] for r in cases('cancel_waiting')),
            'next_player_during_tts_tail_count': sum(r['next_player_started_during_tail'] for r in cases('tts_tail')),
        }
    document = {
        'kind': 'controlled_scheduler_comparison_not_real_model_latency',
        'scheduler_sha256': hashlib.sha256((root / 'Tools/DeliveryAI/scheduler.py').read_bytes()).hexdigest(),
        'baseline_sha256': hashlib.sha256(snapshot.read_bytes()).hexdigest() if baseline_factory else None,
        'workers': 2, 'capacity': 8, 'repeats': args.repeats,
        'simulated_model_ms': args.model_ms, 'observation_window_ms': args.probe_ms,
        'policies': {
            'fifo': 'benchmark-only FIFO with two unrestricted workers',
            'current': 'production Scheduler without monkey patches',
            'baseline': 'unchanged scheduler.py loaded from saved snapshot',
            'candidate': 'benchmark-only player admission replaces waiting ambient; immediate queued cancellation; pause new ambient while players run',
        },
        'limits': ['No real LLM, TTS, HTTP, UE or live service',
                   'TTS tail uses a controlled gate after simulated model end; actual run_job waits futures and server slots enclose run_job',
                   'The 75 ms observation window is a fixture duration, not a measured real-model percentile',
                   'No policy cancels a running callback or releases its worker early'],
        'summary': summary, 'runs': rows,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, ensure_ascii=False, indent=2), encoding='utf-8')
    out.with_suffix('.log').write_text(trace.getvalue(), encoding='utf-8')
    print(json.dumps({'output': str(out), 'summary': summary}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
