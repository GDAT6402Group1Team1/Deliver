"""检查句内声音分段与旧逐句接口、失败和取消的配合，不加载模型"""
import json
import queue
import threading
import unittest
from unittest.mock import patch
import urllib.request

import streaming
from neural_tts import NeuralTtsError
from scheduler import Job
from server import DecisionServer, validate_action, validate_context
from test_sentence_pipeline import request


class Voice:
    supports_audio_stream=True
    voice_name='Xiaofang_B'
    def __init__(self):
        self.full_calls=[]
        self.stream_calls=[]
        self.before_end=lambda cancel: None
    def prepare(self,cancel): return True
    def synthesize(self,text,voice,emotion,cancel):
        self.full_calls.append(text)
        return b'\1\0'*8,24000
    def synthesize_stream(self,text,voice,emotion,cancel,on_chunk,chunk_frames=8):
        self.stream_calls.append(text)
        on_chunk(b'\1\0'*8)
        on_chunk(b'\2\0')
        self.before_end(cancel)
        return 18,24000


class StreamingAudioTests(unittest.TestCase):
    def setUp(self):
        streaming.prepare_resources(local_speech=False)
        self.addCleanup(streaming.close_resources)

    def run_reply(self,voice,context=None,text='你先说。',neural=True,flag='1'):
        context=context or {**request(),'speech_transport':'pcm_chunks_v1'}
        job=Job(context)
        def fallback(context,provider):
            return {'speech':text,'emotion':'Calm'}
        with patch.dict('os.environ',{'DELIVERY_AI_STREAM_AUDIO':flag}), \
             patch('streaming.uses_neural_speech',return_value=neural), \
             patch('streaming.neural_speech_synthesizer',return_value=voice), \
             patch('streaming.speech_synthesizer',return_value=voice):
            streaming.run_job(job,'fixture',validate_action,fallback)
            job.completion.result(timeout=2)
        return job,list(job.events.queue)

    def test_three_sentences_keep_three_sequences_and_one_future_each(self):
        voice=Voice()
        job,events=self.run_reply(voice,text='你先说。我听着。然后呢？')
        self.assertEqual([event['type'] for event in events],
                         ['proposal']+['speech_begin','audio_chunk','audio_chunk','audio_end']*3+['done'])
        self.assertEqual(events[-1]['sentences'],3)
        self.assertEqual([event['sequence'] for event in events if event['type']=='speech_begin'],[0,1,2])
        for sequence in range(3):
            parts=[event for event in events if event.get('sequence')==sequence]
            self.assertEqual(parts[0]['audio_stream_version'],1)
            self.assertEqual([part['offset_bytes'] for part in parts[1:3]],[0,16])
            self.assertEqual([part['chunk_index'] for part in parts[1:3]],[0,1])
            self.assertEqual((parts[-1]['status'],parts[-1]['chunk_count'],parts[-1]['total_pcm_bytes']),
                             ('completed',2,18))
        self.assertEqual(len(voice.stream_calls),3)
        self.assertEqual(voice.full_calls,[])
        self.assertFalse(job.cancel.is_set())

    def test_feature_requires_both_opt_in_and_capable_neural_worker(self):
        cases=((request(),True,'1',True),
               ({**request(),'speech_transport':'pcm_chunks_v1'},True,'',True),
               ({**request(),'speech_transport':'pcm_chunks_v1'},False,'1',True),
               ({**request(),'speech_transport':'pcm_chunks_v1'},True,'1',False))
        for context,neural,flag,supports in cases:
            with self.subTest(neural=neural,flag=flag,supports=supports):
                voice=Voice();voice.supports_audio_stream=supports
                _,events=self.run_reply(voice,context,neural=neural,flag=flag)
                self.assertEqual([event['type'] for event in events],['proposal','sentence','done'])
                self.assertEqual(voice.stream_calls,[])
                self.assertEqual(len(voice.full_calls),1)

    def test_speech_failure_ends_same_sentence_without_duplicate_text_event(self):
        voice=Voice()
        def failed(cancel): raise NeuralTtsError('neural_tts_timeout')
        voice.before_end=failed
        _,events=self.run_reply(voice)
        self.assertEqual([event['type'] for event in events],
                         ['proposal','speech_begin','audio_chunk','audio_chunk','audio_end','done'])
        self.assertEqual(events[-2]['status'],'failed')
        self.assertEqual(events[-2]['audio_error'],'neural_tts_timeout')
        self.assertEqual(events[-1]['sentences'],1)

    def test_cancel_during_stream_has_no_success_end_or_done(self):
        voice=Voice()
        voice.before_end=lambda cancel: cancel.set()
        job,events=self.run_reply(voice)
        self.assertTrue(job.cancel.is_set())
        self.assertFalse(any(event['type'] in ('audio_end','done','sentence') for event in events))
        self.assertTrue(all(job.event_for_client(event) is None for event in events))

    def test_job_waits_for_stream_function_after_last_chunk(self):
        voice=Voice()
        entered,release=threading.Event(),threading.Event()
        def wait(cancel):
            entered.set()
            if not release.wait(2): raise TimeoutError('test_not_released')
        voice.before_end=wait
        job=Job({**request(),'speech_transport':'pcm_chunks_v1'})
        try:
            with patch.dict('os.environ',{'DELIVERY_AI_STREAM_AUDIO':'1'}), \
                 patch('streaming.uses_neural_speech',return_value=True), \
                 patch('streaming.neural_speech_synthesizer',return_value=voice):
                streaming.run_job(job,'fixture',validate_action,lambda *_:{'speech':'你先说。','emotion':'Calm'})
                self.assertTrue(entered.wait(1))
                self.assertFalse(job.completion.done())
                self.assertFalse(any(event['type']=='audio_end' for event in list(job.events.queue)))
                release.set()
                job.completion.result(timeout=2)
                self.assertEqual(list(job.events.queue)[-1]['type'],'done')
        finally:
            release.set()

    def test_transport_validation_rejects_unknown_and_non_string_values(self):
        for value in ('sentence','pcm_chunks_v1'):
            self.assertEqual(validate_context({**request(),'speech_transport':value})['speech_transport'],value)
        for value in ('chunks',1,True,None,[]):
            with self.subTest(value=value),self.assertRaisesRegex(ValueError,'speech transport'):
                validate_context({**request(),'speech_transport':value})

    def test_http_flushes_chunks_before_sentence_synthesis_has_finished(self):
        voice=Voice()
        entered,release=threading.Event(),threading.Event()
        def wait(cancel):
            entered.set()
            if not release.wait(2): raise TimeoutError('test_not_released')
        voice.before_end=wait
        server=DecisionServer(('127.0.0.1',0),'fixture')
        thread=threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with patch.dict('os.environ',{'DELIVERY_AI_STREAM_AUDIO':'1'}), \
                 patch('streaming.uses_neural_speech',return_value=True), \
                 patch('streaming.neural_speech_synthesizer',return_value=voice), \
                 patch('server.decide',return_value={'speech':'你先说。','emotion':'Calm'}):
                context={**request(),'speech_transport':'pcm_chunks_v1'}
                http=urllib.request.Request(
                    f'http://127.0.0.1:{server.server_port}/v1/decisions/stream',
                    data=json.dumps(context).encode(),headers={'Content-Type':'application/json'})
                with urllib.request.urlopen(http,timeout=2) as response:
                    first=[json.loads(response.readline()) for _ in range(4)]
                    self.assertEqual([event['type'] for event in first],
                                     ['proposal','speech_begin','audio_chunk','audio_chunk'])
                    self.assertTrue(entered.wait(1))
                    self.assertFalse(release.is_set())
                    release.set()
                    remaining=[json.loads(line) for line in response if line.strip()]
                    self.assertEqual([event['type'] for event in remaining],['audio_end','done'])
                    self.assertTrue(all(event['request_id']==context['request_id'] for event in first+remaining))
        finally:
            release.set()
            server.shutdown()
            server.server_close()
            thread.join(2)
        self.assertFalse(thread.is_alive())

    def test_cancel_releases_full_event_queue_without_inserting_late_chunk(self):
        job=Job(request())
        for index in range(job.events.maxsize): job.events.put({'type':'previous','index':index})
        entered=threading.Event()
        def emit():
            entered.set()
            job.emit('audio_chunk',sequence=0,chunk_index=0,offset_bytes=0,pcm='AAE=')
        thread=threading.Thread(target=emit)
        thread.start()
        self.assertTrue(entered.wait(1))
        job.cancel_request()
        thread.join(.3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(job.events.qsize(),job.events.maxsize)
        self.assertTrue(all(event['type']=='previous' for event in list(job.events.queue)))

    def test_full_event_queue_reports_error_even_without_room_for_error_event(self):
        job=Job(request())
        for index in range(job.events.maxsize): job.events.put({'type':'previous','index':index})
        with patch('scheduler.time.monotonic',side_effect=[0,3]):
            with self.assertRaises(queue.Full): job.emit('audio_chunk',sequence=0)
        self.assertTrue(job.cancel.is_set())
        self.assertEqual(job.event_for_client(job.events.get())['reason'],'reply_queue_full')


if __name__=='__main__':
    unittest.main()
