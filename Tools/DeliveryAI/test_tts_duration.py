"""检查较长语音从原生回调到父进程的完整结果，测试使用 CPU 样本"""
import base64
import io
import json
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import urllib.request

import test_native_tts as native_fixture
import test_neural_audio_stream as torch_fixture
from test_neural_tts import FakeProcess
from neural_audio_stream import AudioChunkStream
from neural_tts import (CONFIG_PATH, MAX_PROTOCOL_LINE, MAX_STREAM_CHUNKS,
                        NeuralTtsError, QwenSpeechSynthesizer, load_voice_config)
import streaming
from server import DecisionServer
from test_sentence_pipeline import request
from tts import SpeechSynthesizer


class TtsDurationTests(unittest.TestCase):
    def native_client(self, frames, limit):
        client = QwenSpeechSynthesizer()
        client.config['max_pcm_bytes'] = limit
        process = FakeProcess(client)
        client.process, client.ready, client.audio_stream_version = process, True, 1
        engine = native_fixture.NativeTtsTests.engine(self, frames)
        engine.config['max_pcm_bytes'] = limit
        engine.identities = client.identities
        sent = []

        def reply(task_id):
            command = process.commands[-1]
            task = SimpleNamespace(task_id=task_id, voice='male', speaker='Uncle_Fu',
                text=command['text'], chunk_frames=command.get('chunk_frames', 0), cancel=threading.Event())
            def send(message):
                sent.append(message)
                client.messages.put(message)
            try:
                message = engine.synthesize(task, threading.Event(), send)
            except NeuralTtsError as error:
                message = {'type': 'error', 'task_id': task_id, 'code': error.code}
            send(message)

        process.reply = reply
        self.addCleanup(client.close)
        return client, process, engine, sent

    def test_config_accepts_only_eight_or_twelve_seconds(self):
        original = json.loads(CONFIG_PATH.read_text(encoding='utf-8-sig'))
        for limit in (384000, 576000):
            with self.subTest(limit=limit), patch('neural_tts.Path.read_text',
                    return_value=json.dumps({**original, 'max_pcm_bytes': limit})):
                self.assertEqual(load_voice_config()['max_pcm_bytes'], limit)
        for limit in (0, True, 384000.0, 432000, 576002, 960000):
            with self.subTest(limit=limit), patch('neural_tts.Path.read_text',
                    return_value=json.dumps({**original, 'max_pcm_bytes': limit})):
                with self.assertRaisesRegex(NeuralTtsError, 'invalid_config'):
                    load_voice_config()

    def test_nine_second_native_audio_survives_full_and_chunked_protocol(self):
        # 113 个声码帧对应 9.04 秒，覆盖本次 8 秒处被截断的问题
        for chunk_frames in (0, 4, 8):
            with self.subTest(chunk_frames=chunk_frames):
                client, _, engine, sent = self.native_client([3] + [8] * 13 + [6], 576000)
                if chunk_frames:
                    chunks = []
                    result = client.synthesize_stream('行，那咱们和好吧。', 'male', 'Calm',
                        threading.Event(), chunks.append, chunk_frames)
                    self.assertEqual(result, (113 * 3840, 24000))
                    self.assertEqual(sum(map(len, chunks)), result[0])
                else:
                    pcm, rate = client.synthesize('行，那咱们和好吧。', 'male', 'Calm', threading.Event())
                    self.assertEqual((len(pcm), rate), (113 * 3840, 24000))
                    wire = json.dumps(sent[-1], separators=(',', ':')) + '\n'
                    self.assertGreater(len(wire), 524288)
                    self.assertEqual(io.StringIO(wire).readline(MAX_PROTOCOL_LINE + 1), wire)
                self.assertEqual(engine.api.settings['seed'], 20261113)
                self.assertAlmostEqual(engine.api.settings['temperature'], .9)
                self.assertEqual(sum(message['type'] in ('audio', 'audio_end') for message in sent), 1)

    def test_twelve_seconds_keep_first_short_chunk_and_final_tail(self):
        for chunk_frames in (4, 8):
            with self.subTest(chunk_frames=chunk_frames):
                client, _, _, sent = self.native_client([3] + [8] * 18 + [3], 576000)
                chunks = []
                self.assertEqual(client.synthesize_stream('这句话要完整说完。', 'male', 'Calm',
                    threading.Event(), chunks.append, chunk_frames), (576000, 24000))
                self.assertEqual(sum(map(len, chunks)), 576000)
                if chunk_frames == 4:
                    self.assertGreater(len(chunks), 32)
                self.assertLessEqual(len(chunks), MAX_STREAM_CHUNKS)
                self.assertEqual(sent[-1]['total_pcm_bytes'], 576000)
                full_message = {**sent[-1], 'type': 'audio', 'pcm': base64.b64encode(b'\0' * 576000).decode()}
                self.assertLess(len(json.dumps(full_message)) + 1, MAX_PROTOCOL_LINE)

    def test_torch_decoder_twelve_seconds_keeps_reference_separate_and_tail_once(self):
        for chunk_frames in (4, 8):
            with self.subTest(chunk_frames=chunk_frames):
                engine, task, _, talker, decoded, check = torch_fixture.fixture(chunk_frames, reference_frames=2)
                engine.config['max_pcm_bytes'] = 576000
                events = []
                with AudioChunkStream(engine, task, check, events.append) as stream:
                    for step in range(1, 151):
                        talker.step(step)
                    result = stream.finish(576000)
                self.assertEqual(result['total_pcm_bytes'], 576000)
                self.assertEqual(sum(len(base64.b64decode(message['pcm'])) for message in events), 576000)
                self.assertEqual(decoded[-1], 152)
                self.assertEqual(events[-1]['offset_bytes'] + len(base64.b64decode(events[-1]['pcm'])), 576000)

    def test_http_speech_api_keeps_twelve_seconds_and_rejects_one_extra_sample(self):
        synth = SpeechSynthesizer()
        with patch.dict('os.environ', {'DELIVERY_TTS_URL': 'http://test.invalid/tts'}):
            for size in (432000, 576000, 576002):
                with self.subTest(size=size), patch('tts.urllib.request.urlopen', return_value=io.BytesIO(b'\0' * size)):
                    if size > 576000:
                        with self.assertRaisesRegex(ValueError, 'speech_too_large'):
                            synth.synthesize('这句话完整返回。', 'male', 'Calm', threading.Event())
                    else:
                        pcm, rate = synth.synthesize('这句话完整返回。', 'male', 'Calm', threading.Event())
                        self.assertEqual((len(pcm), rate), (size, 24000))

    def test_both_limits_reject_excess_without_reporting_success(self):
        for limit, frames in ((384000, [8] * 12 + [5]), (576000, [8] * 18 + [7])):
            with self.subTest(limit=limit):
                client, _, _, sent = self.native_client(frames, limit)
                chunks = []
                with self.assertRaisesRegex(NeuralTtsError, 'audio_too_long'):
                    client.synthesize_stream('超过上限的声音。', 'male', 'Calm', threading.Event(), chunks.append)
                self.assertLessEqual(sum(map(len, chunks)), limit)
                self.assertEqual(sent[-1]['type'], 'error')
                self.assertFalse(any(message['type'] == 'audio_end' for message in sent))

    def test_cancel_discards_long_reply_before_next_sentence(self):
        client, process, engine, _ = self.native_client([3] + [8] * 18 + [3], 576000)
        cancel, chunks = threading.Event(), []
        def receive(pcm):
            chunks.append(pcm)
            cancel.set()
        with self.assertRaisesRegex(NeuralTtsError, '^cancelled$'):
            client.synthesize_stream('这句在开始播放后取消。', 'male', 'Calm', cancel, receive, 4)
        self.assertEqual(len(chunks), 1)
        self.assertTrue(process.cancel_received.is_set())
        self.assertFalse(process.killed)
        engine.api.frames = [3]
        next_chunks = []
        self.assertEqual(client.synthesize_stream('现在说新的一句。', 'male', 'Calm',
            threading.Event(), next_chunks.append), (11520, 24000))
        self.assertEqual(len(next_chunks), 1)
        self.assertNotEqual(process.commands[0]['task_id'], process.commands[-1]['task_id'])

    def test_http_delivers_three_twelve_second_sentences_with_bounded_queue(self):
        class Voice:
            supports_audio_stream = True
            voice_name = 'Uncle_Fu'
            def prepare(self, cancel):
                return True
            def synthesize_stream(self, text, voice, emotion, cancel, receive):
                remaining = 576000
                while remaining:
                    if cancel.is_set():
                        raise NeuralTtsError('cancelled')
                    size = min(30720, remaining)
                    receive(b'\1\0' * (size // 2))
                    remaining -= size
                return 576000, 24000

        streaming.prepare_resources(local_speech=False)
        self.addCleanup(streaming.close_resources)
        server = DecisionServer(('127.0.0.1', 0), 'fixture')
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with patch.dict('os.environ', {'DELIVERY_AI_STREAM_AUDIO': '1'}), \
                 patch('streaming.uses_neural_speech', return_value=True), \
                 patch('streaming.neural_speech_synthesizer', return_value=Voice()), \
                 patch('server.decide', return_value={'speech': '先说第一句。接着说第二句。最后把第三句说完。', 'emotion': 'Calm'}):
                context = {**request(), 'speech_transport': 'pcm_chunks_v1'}
                http = urllib.request.Request(f'http://127.0.0.1:{server.server_port}/v1/decisions/stream',
                    json.dumps(context).encode(), {'Content-Type': 'application/json'})
                sizes, counts, ends, wire_bytes = {}, {}, [], 0
                with urllib.request.urlopen(http, timeout=3) as response:
                    for line in response:
                        wire_bytes += len(line)
                        self.assertLess(len(line), MAX_PROTOCOL_LINE)
                        event = json.loads(line)
                        self.assertEqual(event['request_id'], context['request_id'])
                        sequence = event.get('sequence')
                        if event['type'] == 'speech_begin':
                            sizes[sequence] = counts[sequence] = 0
                        elif event['type'] == 'audio_chunk':
                            self.assertEqual(event['offset_bytes'], sizes[sequence])
                            self.assertEqual(event['chunk_index'], counts[sequence])
                            sizes[sequence] += len(base64.b64decode(event['pcm'], validate=True))
                            counts[sequence] += 1
                        elif event['type'] == 'audio_end':
                            self.assertEqual(event['status'], 'completed')
                            self.assertEqual(event['total_pcm_bytes'], sizes[sequence])
                            self.assertEqual(event['chunk_count'], counts[sequence])
                            ends.append(sequence)
                        elif event['type'] == 'error':
                            self.fail(str(event))
                self.assertEqual(event['type'], 'done')
                self.assertEqual(event['sentences'], 3)
                self.assertEqual(sizes, {0: 576000, 1: 576000, 2: 576000})
                self.assertEqual(ends, [0, 1, 2])
                self.assertGreater(wire_bytes, 2304000)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(2)
        self.assertFalse(thread.is_alive())


if __name__ == '__main__':
    unittest.main()
