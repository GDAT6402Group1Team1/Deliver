"""用 CPU 模拟模型回调，检查分段尾音、参考录音、取消和退出，不加载 Torch 或模型"""
import base64
from contextlib import nullcontext
import threading
import time
from types import SimpleNamespace
import unittest

import numpy as np

from neural_audio_stream import AudioChunkStream, AudioStreamComplete
from neural_audio_sender import AudioChunkSender
from neural_tts import NeuralTtsError
from neural_tts_worker import QwenEngine, QwenVoiceEngines, WorkerRequests, check_cancel


class CpuTensor:
    def __init__(self, values, copied=None):
        self.values = np.asarray(values)
        self.shape, self.ndim = self.values.shape, self.values.ndim
        self.device = 'cpu'
        self.copied = copied

    def __len__(self):
        return len(self.values)

    def __getitem__(self, key):
        return CpuTensor(self.values[key], self.copied)

    def detach(self):
        return self

    def clone(self):
        return CpuTensor(self.values.copy())

    def to(self, _device):
        return self

    def item(self):
        return self.values.item()

    def unsqueeze(self, axis):
        return CpuTensor(np.expand_dims(self.values, axis), self.copied)

    def float(self):
        return CpuTensor(self.values.astype(np.float32), self.copied)

    def cpu(self):
        if self.copied:
            self.copied(self.values.size)
        return self

    def numpy(self):
        return self.values


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
    copied = []

    def decode(codes, return_dict):
        self_shape = codes.shape
        assert return_dict and len(self_shape) == 3 and self_shape[0] == 1 and self_shape[2] == 16
        frames = self_shape[1]
        decoded.append(frames)
        # 参考录音与本句使用不同数值，输出包含参考录音时可以直接检出
        pcm = np.concatenate((np.full(reference_frames * 1920, -.75, np.float32),
                              np.full((frames - reference_frames) * 1920, .25, np.float32)))
        def copied_to_cpu(count):
            copied.append(count)
            if getattr(engine, 'after_copy', None):
                engine.after_copy()
        return SimpleNamespace(audio_values=[CpuTensor(pcm, copied_to_cpu)])

    def public_decode(items):
        engine.public_decode_calls += 1
        output = decode(items[0]['audio_codes'].unsqueeze(0), True)
        return [output.audio_values[0].numpy()], 24000

    engine = SimpleNamespace(
        np=np, torch=SimpleNamespace(inference_mode=nullcontext,
            stack=lambda xs: CpuTensor(np.stack([x.values for x in xs])),
            cat=lambda xs, dim: CpuTensor(np.concatenate([x.values for x in xs], axis=dim)),
            equal=lambda left, right: bool(np.array_equal(left.values, right.values))),
        model=SimpleNamespace(model=SimpleNamespace(talker=talker,
            config=SimpleNamespace(talker_config=SimpleNamespace(codec_eos_token_id=999)),
            speech_tokenizer=SimpleNamespace(decode=public_decode,
                get_model_type=lambda: 'qwen3_tts_tokenizer_12hz', get_decode_upsample_rate=lambda: 1920,
                model=SimpleNamespace(decode=decode, get_output_sample_rate=lambda: 24000)))),
        copied=copied, public_decode_calls=0,
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
        engine.after_copy = task.cancel.set
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
            with AudioChunkStream(engine, task, check, receive) as stream:
                for step in range(1, 7):
                    talker.step(step)
                stream.finish(6 * 3840)
        self.assertEqual(len(events), 1)
        self.assertTrue(talker.removed)

    def test_callback_failure_removes_hook_and_next_task_can_generate(self):
        engine, task, _, talker, _, check = fixture()
        def failed(_):
            raise OSError('closed pipe')
        with self.assertRaisesRegex(NeuralTtsError, 'callback_failed'):
            with AudioChunkStream(engine, task, check, failed) as stream:
                for step in range(1, 5):
                    talker.step(step)
                stream.finish(4 * 3840)
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
                    return engine.model.model.speech_tokenizer.decode(
                        [{'audio_codes': CpuTensor(np.ones((6, 16), dtype=np.int64))}])
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
                    self.assertLessEqual(len(events), 1)
                self.assertTrue(all(row['voice_identity'] == engines.identities['male'] for row in events))
                self.assertTrue(talker.removed)
                self.assertIs(talker.generate, generate)

    def test_only_new_samples_cross_to_cpu_and_final_decode_is_skipped(self):
        engine, task, _, talker, decoded, check = fixture(4, 3)
        tokenizer = engine.model.model.speech_tokenizer
        original = tokenizer.decode
        engine.synchronize = lambda: self.fail('分段解码不应同步整个 CUDA 设备')
        events = []
        with AudioChunkStream(engine, task, check, events.append) as stream:
            for step in range(1, 11):
                talker.step(step)
            codes = CpuTensor(np.concatenate((np.zeros((3, 16)), np.ones((10, 16)))))
            with self.assertRaises(AudioStreamComplete) as complete:
                tokenizer.decode([{'audio_codes': codes}])
            self.assertIs(complete.exception.stream, stream)
            result = stream.finish()
        self.assertIs(tokenizer.decode, original)
        self.assertEqual(decoded, [7, 11, 13])
        self.assertEqual(engine.copied, [4 * 1920, 4 * 1920, 2 * 1920])
        self.assertEqual(engine.public_decode_calls, 0)
        self.assertEqual(result['stream_metrics']['copied_samples'], 10 * 1920)
        self.assertEqual(result['stream_metrics']['decode_calls'], 3)
        # 接下来普通整句调用仍用原方法
        output, rate = tokenizer.decode([{'audio_codes': codes}])
        self.assertEqual((engine.public_decode_calls, rate, len(output[0])), (1, 24000, 13 * 1920))

    def test_final_code_mismatch_does_not_report_success(self):
        engine, task, _, talker, _, check = fixture()
        original = engine.model.model.speech_tokenizer.decode
        with self.assertRaisesRegex(NeuralTtsError, 'layout_changed'):
            with AudioChunkStream(engine, task, check, lambda _: None):
                talker.step(1)
                engine.model.model.speech_tokenizer.decode(
                    [{'audio_codes': CpuTensor(np.full((1, 16), 2))}])
        self.assertIs(engine.model.model.speech_tokenizer.decode, original)

    def test_output_queue_does_not_block_generation_or_send_cancelled_pending_chunks(self):
        engine, task, _, talker, _, check = fixture()
        entered, release = threading.Event(), threading.Event()
        events, threads = [], []
        owner_thread = threading.get_ident()
        def receive(message):
            events.append(message)
            threads.append(threading.get_ident())
            entered.set()
            self.assertTrue(release.wait(2))
        try:
            with AudioChunkStream(engine, task, check, receive):
                for step in range(1, 5):
                    talker.step(step)
                self.assertTrue(entered.wait(1))
                # 第一块仍在发送，后面八帧可以继续生成
                for step in range(5, 13):
                    talker.step(step)
                self.assertEqual(talker.callback.__self__.last_step, 12)
                task.cancel.set()
                release.set()
        finally:
            release.set()
        self.assertEqual(len(events), 1)
        self.assertNotEqual(threads[0], owner_thread)

    def test_bounded_sender_propagates_cancel_while_queue_is_full(self):
        cancel, entered, release = threading.Event(), threading.Event(), threading.Event()
        def check():
            if cancel.is_set():
                raise NeuralTtsError('cancelled')
        def receive(_):
            entered.set()
            self.assertTrue(release.wait(2))
        sender = AudioChunkSender(receive, check, capacity=1)
        errors = []
        def enqueue_third():
            try:
                sender.enqueue({}, b'\0\0')
            except NeuralTtsError as error:
                errors.append(error.code)
        try:
            sender.enqueue({}, b'\0\0')
            self.assertTrue(entered.wait(1))
            sender.enqueue({}, b'\0\0')
            waiting = threading.Thread(target=enqueue_third)
            waiting.start()
            cancel.set()
            waiting.join(1)
            self.assertFalse(waiting.is_alive())
            self.assertEqual(errors, ['cancelled'])
        finally:
            release.set()
            sender.close()


if __name__ == '__main__':
    unittest.main()
