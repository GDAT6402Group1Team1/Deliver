"""检查分句、语音合成与模型生成的配合，以及打断后的处理"""
import json
import queue
import threading
import unittest
from unittest.mock import patch

from scheduler import Job
from server import validate_action
from streaming import SentenceParser, ReplyFormatError, split_speech, run_job, prepare_resources, close_resources
from test_server import context


def request():
    data=context()
    data['candidates']=[{'action':'Wait','target_id':'','duration':1,'evidence_ids':[]}]
    return data


class SentenceSplitTests(unittest.TestCase):
    def test_game_reply_and_followup_keep_all_text_and_order(self):
        text='刚才没听清是不是说我。谁挡了路还没说清，不过他道歉了，我先让他送货。'
        events=[]
        parser=SentenceParser(request(),validate_action,lambda kind,data:events.append((kind,data)))
        encoded=json.dumps({'candidate_index':0,'speech':text,'emotion':'Calm'},ensure_ascii=False)
        parser.feed(encoded[:-1])
        self.assertEqual(events,[])
        parser.feed(encoded[-1:])
        self.assertEqual([event[0] for event in events],['proposal','sentence','sentence'])
        parser.feed(json.dumps({'speech':'你还有事吗？'},ensure_ascii=False))
        parser.finish()
        sentences=[data for kind,data in events if kind=='sentence']
        self.assertEqual([item['sequence'] for item in sentences],[0,1,2])
        self.assertEqual(''.join(item['text'] for item in sentences),text+'你还有事吗？')

    def test_quotes_decimals_and_abbreviations_stay_together(self):
        cases=[
            ('他说“别挡路。先送货！”我先走。你呢？',['他说“别挡路。先送货！”我先走。','你呢？']),
            ('Dr. Li花了3.14元。U.S.是缩写。',['Dr. Li花了3.14元。','U.S.是缩写。']),
            ('Wait... That is fine. Go!',['Wait... That is fine.',' Go!']),
            ('你说什么？！ 先让开。  ',['你说什么？！',' 先让开。  ']),
            ('他说"Go!"，我照办。', ['他说"Go!"，我照办。'])]
        for text,expected in cases:
            with self.subTest(text=text):
                parts=split_speech(text)
                self.assertEqual(parts,expected)
                self.assertEqual(''.join(parts),text)

    def test_oversized_record_fails_before_starting_speech(self):
        for text in ('一。二。三。四。','字'*121):
            events=[]
            parser=SentenceParser(request(),validate_action,lambda *event:events.append(event))
            with self.assertRaises(ValueError):
                parser.feed(json.dumps({'candidate_index':0,'speech':text,'emotion':'Calm'}))
            self.assertEqual(events,[])

    def test_later_record_overflow_reports_failure(self):
        events=[]
        parser=SentenceParser(request(),validate_action,lambda kind,data:events.append((kind,data)))
        parser.feed(json.dumps({'candidate_index':0,'speech':'一。二。三。','emotion':'Calm'}))
        with self.assertRaisesRegex(ValueError,'too_many_sentences'):
            parser.feed(json.dumps({'speech':'四。'}))
        self.assertEqual([data['text'] for kind,data in events if kind=='sentence'],['一。','二。','三。'])

    def test_character_limit_counts_all_model_records(self):
        events=[]
        parser=SentenceParser(request(),validate_action,lambda kind,data:events.append((kind,data)))
        parser.feed(json.dumps({'candidate_index':0,'speech':'字'*70,'emotion':'Calm'}))
        with self.assertRaisesRegex(ValueError,'reply_too_long'):
            parser.feed(json.dumps({'speech':'字'*51}))
        self.assertEqual([data['text'] for kind,data in events if kind=='sentence'],['字'*70])


class SpeechPipelineTests(unittest.TestCase):
    def test_invalid_followup_keeps_the_checked_first_sentence_and_waits_for_its_audio(self):
        followups = [json.dumps({'speech':'二。三。四。'}),
                     json.dumps({'candidate_index':0,'speech':'追加动作。'}),
                     '{"speech":', '[]']
        for followup in followups:
            with self.subTest(followup=followup):
                started, release = threading.Event(), threading.Event()
                job = Job(request())
                class Voice:
                    def synthesize(self, text, voice, emotion, cancel):
                        started.set()
                        if not release.wait(2):
                            raise TimeoutError('test voice was not released')
                        return b'\x01\x00'*160,16000
                def model(prompt, cancel, on_delta):
                    on_delta(json.dumps({'candidate_index':0,'speech':'我听见了。','emotion':'Calm'}))
                    self.assertTrue(started.wait(1))
                    on_delta(followup)
                prepare_resources(local_speech=False)
                try:
                    with patch('streaming.stream_api',side_effect=model), \
                         patch('streaming.uses_neural_speech',return_value=False), \
                         patch('streaming.speech_synthesizer',return_value=Voice()):
                        run_job(job,'llm',validate_action,None)
                        self.assertFalse(job.cancel.is_set())
                        self.assertFalse(job.completion.done(), '首句声音仍在合成')
                        release.set()
                        job.completion.result(timeout=2)
                    events=list(job.events.queue)
                    self.assertEqual([event['type'] for event in events],['proposal','sentence','done'])
                    self.assertEqual(events[1]['text'],'我听见了。')
                    self.assertTrue(events[1]['pcm'])
                    self.assertEqual(events[-1]['sentences'],1)
                finally:
                    release.set();close_resources()

    def test_transport_timeout_and_external_cancel_still_discard_the_first_audio(self):
        for failure in ('timeout','transport','external_cancel'):
            with self.subTest(failure=failure):
                started, release = threading.Event(), threading.Event()
                job = Job(request())
                class Voice:
                    def synthesize(self, text, voice, emotion, cancel):
                        started.set()
                        if not release.wait(2):
                            raise TimeoutError('test voice was not released')
                        return b'\x01\x00'*160,16000
                def model(prompt, cancel, on_delta):
                    on_delta(json.dumps({'candidate_index':0,'speech':'我听见了。','emotion':'Calm'}))
                    self.assertTrue(started.wait(1))
                    if failure == 'timeout':
                        raise TimeoutError('model timeout')
                    if failure == 'transport':
                        raise OSError('connection lost')
                    job.cancel_request()
                    on_delta(json.dumps({'speech':'二。三。四。'}))
                prepare_resources(local_speech=False)
                try:
                    with patch('streaming.stream_api',side_effect=model), \
                         patch('streaming.uses_neural_speech',return_value=False), \
                         patch('streaming.speech_synthesizer',return_value=Voice()):
                        with self.assertRaises((TimeoutError,OSError,ReplyFormatError)):
                            run_job(job,'llm',validate_action,None)
                        self.assertTrue(job.cancel.is_set())
                        self.assertFalse(job.completion.done(), '取消后仍要等实际合成返回')
                        release.set()
                        job.completion.result(timeout=2)
                    events=list(job.events.queue)
                    self.assertFalse(any(event['type'] in ('sentence','done') for event in events))
                    if failure == 'external_cancel':
                        self.assertTrue(all(job.event_for_client(event) is None for event in events))
                    else:
                        self.assertEqual(job.event_for_client(events[0])['type'],'error')
                finally:
                    release.set();close_resources()

    def test_invalid_initial_action_still_fails_without_starting_tts(self):
        job = Job(request())
        def model(prompt,cancel,on_delta):
            on_delta(json.dumps({'candidate_index':99,'speech':'我听见了。','emotion':'Calm'}))
        prepare_resources(local_speech=False)
        try:
            with patch('streaming.stream_api',side_effect=model), \
                 patch('streaming.uses_neural_speech',return_value=False), \
                 patch('streaming.speech_synthesizer',side_effect=AssertionError('unexpected TTS')):
                with self.assertRaisesRegex(ReplyFormatError,'invalid_candidate'):
                    run_job(job,'llm',validate_action,None)
            self.assertTrue(job.completion.done())
            self.assertEqual([event['type'] for event in list(job.events.queue)],['error'])
        finally:
            close_resources()

    def test_first_audio_is_ready_while_model_continues_and_tts_finishes_out_of_order(self):
        continue_model=threading.Event()
        continue_second_voice=threading.Event()
        model_finished=threading.Event()
        errors=[]
        class Model:
            def stream(self,prompt,instructions,cancel,on_delta,model=None,measure=None):
                on_delta(json.dumps({'candidate_index':0,'speech':'先让开。再说话。','emotion':'Calm'}))
                if not continue_model.wait(2):raise RuntimeError('test_model_timeout')
                on_delta(json.dumps({'speech':'你听见了吗？'}))
                model_finished.set()
            def close(self):pass
        class Voice:
            def synthesize(self,text,voice,emotion,cancel):
                if text=='再说话。' and not continue_second_voice.wait(2):
                    raise RuntimeError('test_voice_timeout')
                return b'\x01\x00'*1600,16000
        job=Job(request())
        def run():
            try:run_job(job,'codex-local',validate_action,None)
            except Exception as error:errors.append(error)
        prepare_resources(local_speech=False)
        with patch('streaming.AppServer',Model),patch('streaming.speech_synthesizer',return_value=Voice()):
            worker=threading.Thread(target=run,daemon=True)
            try:
                worker.start()
                self.assertEqual(job.events.get(timeout=1)['type'],'proposal')
                first=job.events.get(timeout=1)
                self.assertEqual((first['type'],first['sequence']),('sentence',0))
                self.assertFalse(model_finished.is_set())
                continue_model.set()
                third=job.events.get(timeout=1)
                self.assertEqual(third['sequence'],2)
                self.assertTrue(model_finished.wait(1))
                continue_second_voice.set()
                second=job.events.get(timeout=1)
                self.assertEqual(second['sequence'],1)
                done=job.events.get(timeout=1)
                self.assertEqual((done['type'],done['sentences']),('done',3))
                self.assertEqual(''.join(item['text'] for item in sorted([first,third,second],key=lambda item:item['sequence'])),
                                 '先让开。再说话。你听见了吗？')
            finally:
                continue_model.set();continue_second_voice.set();worker.join(3);close_resources()
            self.assertFalse(worker.is_alive())
            self.assertEqual(errors,[])

    def test_cancel_discards_late_audio_from_both_workers(self):
        voices_started=threading.Barrier(3)
        errors=[]
        class Model:
            def stream(self,prompt,instructions,cancel,on_delta,model=None,measure=None):
                on_delta(json.dumps({'candidate_index':0,'speech':'先让开。再说话。','emotion':'Calm'}))
                cancel.wait(2)
                raise RuntimeError('cancelled')
            def close(self):pass
        class Voice:
            def synthesize(self,text,voice,emotion,cancel):
                voices_started.wait(timeout=2)
                cancel.wait(2)
                return b'\x01\x00'*1600,16000
        job=Job(request())
        def run():
            try:run_job(job,'codex-local',validate_action,None)
            except RuntimeError as error:errors.append(str(error))
        prepare_resources(local_speech=False)
        with patch('streaming.AppServer',Model),patch('streaming.speech_synthesizer',return_value=Voice()):
            worker=threading.Thread(target=run,daemon=True)
            try:
                worker.start()
                self.assertEqual(job.events.get(timeout=1)['type'],'proposal')
                voices_started.wait(timeout=2)
                job.cancel.set()
                worker.join(2)
                self.assertFalse(worker.is_alive())
                self.assertEqual(errors,['cancelled'])
                with self.assertRaises(queue.Empty):job.events.get_nowait()
            finally:job.cancel.set();worker.join(3);close_resources()


if __name__=='__main__':unittest.main()
