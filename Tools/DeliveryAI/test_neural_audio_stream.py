"""用 CPU 模拟模型回调，检查分段尾音、参考录音、取消和退出，不加载 Torch 或模型"""
import base64
from contextlib import nullcontext
import threading
import time
from types import SimpleNamespace
import unittest

import numpy as np

from neural_audio_stream import AudioChunkStream
from neural_tts import NeuralTtsError
from neural_tts_worker import QwenEngine, QwenVoiceEngines, WorkerRequests, check_cancel


class CpuTensor:
    def __init__(self, values):
        self.values = np.asarray(values)
        self.shape, self.ndim = self.values.shape, self.values.ndim
        self.device = 'cpu'

    def __len__(self):
        return len(self.values)

    def __getitem__(self, key):
        return CpuTensor(self.values[key])

    def detach(self):
        return self

    def clone(self):
        return CpuTensor(self.values.copy())

    def to(self, _device):
        return self

    def item(self):
        return self.values.item()


class CpuTalker:
    def __init__(self):
        self.callback = None
        self.removed = False

    def register_forward_hook(self, callback, with_kwargs):
        assert with_kwargs and self.callback is None
        self.callback, self.removed = callback, False
        def remove():
            self.callback, self.removed = None, True
        return SimpleNamespace(remove=remove)

    def step(self, index, code=1):
        self.callback(self, (), {}, SimpleNamespace(
            hidden_states=(None, CpuTensor(np.full((1, 16), code))), generation_step=index))


def fixture(chunk_frames=4, reference_frames=0):
    task = SimpleNamespace(task_id='a' * 32, speaker='test_voice', chunk_frames=chunk_frames,
                           cancel=threading.Event())
    closed = threading.Event()
    talker = CpuTalker()
    decoded = []

    def decode(items):
        frames = len(items[0]['audio_codes'])
        decoded.append(frames)
        # 参考录音与本句使用不同数值，输出包含参考录音时可以直接检出
        pcm = np.concatenate((np.full(reference_frames * 1920, -.75, np.float32),
                              np.full((frames - reference_frames) * 1920, .25, np.float32)))
        return [pcm], 24000

    engine = SimpleNamespace(
        np=np, torch=SimpleNamespace(inference_mode=nullcontext,
            stack=lambda xs: CpuTensor(np.stack([x.values for x in xs])),
            cat=lambda xs, dim: CpuTensor(np.concatenate([x.values for x in xs], axis=dim))),
        model=SimpleNamespace(model=SimpleNamespace(talker=talker,
            config=SimpleNamespace(talker_config=SimpleNamespace(codec_eos_token_id=999)),
            speech_tokenizer=SimpleNamespace(decode=decode))),
        config={'max_pcm_bytes': 384000, 'max_new_tokens': 256}, synchronize=lambda: None)
    if reference_frames:
        engine.prompt = [SimpleNamespace(icl_mode=True, ref_code=CpuTensor(np.zeros((reference_frames, 16))))]
    check = lambda: check_cancel(task.cancel, closed, time.monotonic() + 10)
    return engine, task, closed, talker, decoded, check


class NeuralAudioStreamTests(unittest.TestCase):
    def test_full_mode_does_not_install_hook_or_require_callback(self):
        engine, task, _, talker, decoded, check = fixture(0)
        with AudioChunkStream(engine, task, check, None):
            self.assertIsNone(talker.callback)
        self.assertEqual(decoded, [])

    def test_chunk_tail_and_reference_audio_are_emitted_once(self):
        for frames, count, reference in ((4, 6, 0), (8, 10, 2), (8, 2, 2), (4, 4, 0)):
            with self.subTest(frames=frames, count=count, reference=reference):
                engine, task, _, talker, _, check = fixture(frames, reference)
                events = []
                with AudioChunkStream(engine, task, check, events.append) as stream:
                    for step in range(1, count + 1):
                        talker.step(step)
                    result = stream.finish(count * 3840)
                    with self.assertRaisesRegex(NeuralTtsError, 'length_mismatch'):
                        stream.finish(count * 3840)
                self.assertTrue(talker.removed)
                pcm = b''.join(base64.b64decode(row['pcm']) for row in events)
                self.assertEqual(len(pcm), count * 3840)
                self.assertTrue(np.all(np.frombuffer(pcm, '<i2') == 8192))
                self.assertEqual(result['chunk_count'], len(events))
                self.assertEqual(result['total_pcm_bytes'], len(pcm))
                offset = 0
                for index, row in enumerate(events):
                    self.assertEqual((row['chunk_index'], row['offset_bytes']), (index, offset))
                    offset += len(base64.b64decode(row['pcm']))

    def test_cancel_during_decode_does_not_send_late_audio(self):
        engine, task, _, talker, _, check = fixture()
        engine.synchronize = task.cancel.set
        events = []
        with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
            with AudioChunkStream(engine, task, check, events.append):
                for step in range(1, 5):
                    talker.step(step)
        self.assertEqual(events, [])
        self.assertTrue(talker.removed)

    def test_cancel_after_first_chunk_does_not_emit_tail_or_success(self):
        engine, task, _, talker, _, check = fixture()
        events = []
        def receive(message):
            events.append(message)
            task.cancel.set()
        with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
            with AudioChunkStream(engine, task, check, receive):
                for step in range(1, 7):
                    talker.step(step)
        self.assertEqual(len(events), 1)
        self.assertTrue(talker.removed)

    def test_callback_failure_removes_hook_and_next_task_can_generate(self):
        engine, task, _, talker, _, check = fixture()
        def failed(_):
            raise OSError('closed pipe')
        with self.assertRaisesRegex(NeuralTtsError, 'callback_failed'):
            with AudioChunkStream(engine, task, check, failed):
                for step in range(1, 5):
                    talker.step(step)
        self.assertTrue(talker.removed)
        events = []
        with AudioChunkStream(engine, task, check, events.append) as stream:
            talker.step(1)
            self.assertEqual(stream.finish(3840)['total_pcm_bytes'], 3840)
        self.assertEqual(len(events), 1)

    def test_shutdown_and_timeout_stop_before_callback(self):
        for reason in ('closed', 'timeout'):
            engine, task, closed, talker, _, check = fixture()
            if reason == 'closed':
                closed.set()
            else:
                check = lambda: check_cancel(task.cancel, closed, 0)
            events = []
            with self.assertRaisesRegex(NeuralTtsError, 'cancelled|timeout'):
                with AudioChunkStream(engine, task, check, events.append):
                    self.fail('已结束的任务不应进入生成')
            self.assertIsNone(talker.callback)
            self.assertEqual(events, [])

    def test_length_mismatch_and_audio_limit_do_not_report_success(self):
        for limit, total, error in ((384000, 3842, 'length_mismatch'), (3000, 3840, 'audio_too_long')):
            engine, task, _, talker, _, check = fixture()
            engine.config['max_pcm_bytes'] = limit
            events = []
            with self.assertRaisesRegex(NeuralTtsError, error):
                with AudioChunkStream(engine, task, check, events.append) as stream:
                    talker.step(1)
                    stream.finish(total)
            self.assertTrue(talker.removed)

    def test_out_of_order_frames_and_frames_after_eos_are_rejected(self):
        for sequence in (((2, 1),), ((1, 999), (2, 1))):
            engine, task, _, talker, _, check = fixture()
            events = []
            with self.assertRaisesRegex(NeuralTtsError, 'layout_changed'):
                with AudioChunkStream(engine, task, check, events.append):
                    for index, code in sequence:
                        talker.step(index, code)
            self.assertEqual(events, [])
            self.assertTrue(talker.removed)

    def test_worker_only_accepts_explicit_supported_chunk_sizes(self):
        for frames in (None, True, '4', 1, 16):
            replies = []
            requests = WorkerRequests(replies.append, {'male': 'test_voice'})
            requests.receive({'type': 'synthesize', 'task_id': 'a' * 32, 'text': '你先说。',
                              'voice': 'male', 'emotion': 'Calm', 'chunk_frames': frames})
            self.assertEqual(replies[0]['code'], 'neural_tts_invalid_chunk_frames')
            self.assertIsNone(requests.current)
        for frames in (0, 4, 8):
            requests = WorkerRequests(lambda _: None, {'male': 'test_voice'})
            requests.receive({'type': 'synthesize', 'task_id': 'a' * 32, 'text': '你先说。',
                              'voice': 'male', 'emotion': 'Calm', 'chunk_frames': frames})
            self.assertEqual(requests.pending.get_nowait().chunk_frames, frames)

    def test_engine_preserves_identity_and_requires_eos_before_end(self):
        for has_eos in (True, False):
            with self.subTest(has_eos=has_eos):
                source, task, closed, talker, _, _ = fixture()
                engine = QwenEngine.__new__(QwenEngine)
                engine.__dict__.update(vars(source))
                engine.config.update(seed=20261113, synthesis_timeout_seconds=20)
                engine.StoppingCriteria, engine.StoppingCriteriaList = object, list
                engine.torch.manual_seed = lambda _: None
                engine.torch.cuda = SimpleNamespace(manual_seed_all=lambda _: None)
                talker.code_predictor = SimpleNamespace(generate=lambda **_: None)
                def generate(**_):
                    for step in range(1, 7):
                        talker.step(step)
                    return SimpleNamespace(sequences=np.array([999 if has_eos else 1]))
                talker.generate = generate
                def voice(**_):
                    talker.generate()
                    return [np.full(6 * 1920, .25, np.float32)], 24000
                engine.model.generate_custom_voice = voice
                task.voice, task.text = 'male', '先听我说。'
                engines = QwenVoiceEngines.__new__(QwenVoiceEngines)
                engines.engines = {'male': engine}
                engines.identities = {'male': {'provider': 'qwen', 'speaker': task.speaker}}
                events = []
                if has_eos:
                    result = engines.synthesize(task, closed, events.append)
                    self.assertEqual((result['type'], result['total_pcm_bytes']), ('audio_end', 6 * 3840))
                    self.assertEqual(result['voice_identity'], engines.identities['male'])
                    self.assertEqual(len(events), 2)
                else:
                    with self.assertRaisesRegex(NeuralTtsError, 'token_limit'):
                        engines.synthesize(task, closed, events.append)
                    self.assertEqual(len(events), 1)
                self.assertTrue(all(row['voice_identity'] == engines.identities['male'] for row in events))
                self.assertTrue(talker.removed)
                self.assertIs(talker.generate, generate)


if __name__ == '__main__':
    unittest.main()
