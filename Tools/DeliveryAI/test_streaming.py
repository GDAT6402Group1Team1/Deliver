"""验证完整句子、队列优先级、取消与真实 HTTP 分段传输"""
import copy
import io
import json
import queue
import threading
import time
import unittest
import urllib.request
import uuid
from types import SimpleNamespace
from unittest.mock import Mock, patch
from test_server import context
from server import DecisionServer, validate_action, validate_context
from scheduler import Job, Scheduler
from streaming import SentenceParser, compact_context, prepare_resources, close_resources, speech_synthesizer, run_job
from tts import SpeechSynthesizer
from app_server import AppServer

def request():
    data=context()
    data['evidence'][0]['kind']='PlayerSpeech'
    data['candidates']=[{'action':'Wait','target_id':'','duration':1,'evidence_ids':[]}]
    return data

class ContextTests(unittest.TestCase):
    def test_action_reply_finishes_only_the_matching_player_message(self):
        data=request()
        player=data['evidence'][0]
        action={'event_id':str(uuid.uuid4()),'subject_id':'','kind':'ActionReply',
                'description':'ReleaseVehicle Succeeded','reply_to_event_id':player['event_id']}
        data['evidence'].append(action)
        compact=compact_context(data)
        self.assertIsNone(compact['pending_player_message'])
        self.assertEqual(compact['recent_self_speech_ids'],[])
        self.assertEqual(compact['last_complete_player_reply_id'],action['event_id'])
        self.assertEqual(compact['new_event_ids'],[])
        repeated={**player,'event_id':str(uuid.uuid4())}
        data['evidence'].append(repeated)
        self.assertEqual(compact_context(data)['pending_player_message']['event_id'],repeated['event_id'])

    def test_action_finished_alone_is_not_a_reply(self):
        data=request()
        player=data['evidence'][0]
        data['evidence'].append({'event_id':str(uuid.uuid4()),'subject_id':'','kind':'ActionFinished',
                                'description':'MoveTo Failed','reply_to_event_id':player['event_id']})
        self.assertEqual(compact_context(data)['pending_player_message']['event_id'],player['event_id'])

    def test_current_message_follows_player_topic_change(self):
        data=request()
        apology=data['evidence'][0]
        apology['description']='对不起，刚才是我太急了。'
        data['evidence'].append({'event_id':str(uuid.uuid4()),'subject_id':'','kind':'SelfSpeech',
                                'description':'行，听见你道歉了，先送货吧。','reply_to_event_id':apology['event_id']})
        provoke={**apology,'event_id':str(uuid.uuid4()),'description':'他刚才是不是在说你？你们到底谁有理？'}
        data['evidence'].append(provoke)
        data['evidence'].append({'event_id':str(uuid.uuid4()),'subject_id':apology['subject_id'],
                                'kind':'Seen','description':'玩家仍站在面前'})
        compact=compact_context(data)
        self.assertEqual(compact['current_player_message']['event_id'],provoke['event_id'])
        self.assertEqual(compact['pending_player_message']['description'],provoke['description'])
        self.assertFalse(compact['current_player_message']['has_complete_reply'])
        self.assertTrue(any(item['event_id']==apology['event_id'] for item in compact['evidence']))
        data['evidence'].append({'event_id':str(uuid.uuid4()),'subject_id':'','kind':'SelfSpeech',
                                'description':'他说的是车挡路。','reply_to_event_id':provoke['event_id']})
        compact=compact_context(data)
        self.assertTrue(compact['current_player_message']['has_complete_reply'])
        self.assertIsNone(compact['pending_player_message'])
        self.assertEqual(compact['new_event_ids'],[])

    def test_no_player_message_keeps_current_message_empty(self):
        data=request();data['evidence'][0]['kind']='Speech'
        compact=compact_context(data)
        self.assertIsNone(compact['current_player_message'])
        self.assertIsNone(compact['pending_player_message'])
        self.assertEqual(compact['new_event_ids'],[data['evidence'][0]['event_id']])

    def test_answered_player_message_leaves_new_npc_speech_as_the_new_event(self):
        data=request()
        player=data['evidence'][0]
        player['description']='刚才是我撞的，对不起。'
        reply={'event_id':str(uuid.uuid4()),'subject_id':'','kind':'SelfSpeech',
               'description':'看见了，下回看着点。','reply_to_event_id':player['event_id']}
        neighbor={'event_id':str(uuid.uuid4()),'subject_id':str(uuid.uuid4()),'kind':'Speech',
                  'description':'你自己不也站在路中间吗？'}
        data['evidence'].extend([reply,neighbor])
        original=copy.deepcopy(data)
        compact=compact_context(data)
        self.assertIsNone(compact['pending_player_message'])
        self.assertNotIn('description',compact['current_player_message'])
        self.assertTrue(compact['current_player_message']['has_complete_reply'])
        self.assertEqual(compact['recent_self_speech_ids'],[reply['event_id']])
        self.assertEqual(compact['last_complete_player_reply_id'],reply['event_id'])
        self.assertEqual(compact['new_event_ids'],[neighbor['event_id']])
        self.assertEqual(compact['evidence'][-1]['description'],neighbor['description'])
        self.assertEqual(data,original)

    def test_same_words_with_a_new_event_id_still_need_a_reply(self):
        data=request()
        first=data['evidence'][0]
        first['description']='他刚才是不是在说你？'
        reply={'event_id':str(uuid.uuid4()),'subject_id':'','kind':'SelfSpeech',
               'description':'说的是车，不是我。','reply_to_event_id':first['event_id']}
        repeated={**first,'event_id':str(uuid.uuid4())}
        data['evidence'].extend([reply,repeated])
        compact=compact_context(data)
        self.assertEqual(compact['pending_player_message']['event_id'],repeated['event_id'])
        self.assertEqual(compact['pending_player_message']['description'],first['description'])
        self.assertFalse(compact['current_player_message']['has_complete_reply'])
        self.assertEqual(compact['new_event_ids'],[repeated['event_id']])
        self.assertEqual(compact['last_complete_player_reply_id'],reply['event_id'])

    def test_only_the_complete_reply_to_this_player_event_finishes_the_reply(self):
        for kind,reply_to in [('SelfSpeech',''),('SelfSpeech',str(uuid.uuid4())),('Speech','current')]:
            with self.subTest(kind=kind,reply_to=reply_to):
                data=request()
                player=data['evidence'][0]
                spoken={'event_id':str(uuid.uuid4()),'subject_id':'','kind':kind,
                        'description':'他刚才说的是车。',
                        'reply_to_event_id':player['event_id'] if reply_to=='current' else reply_to}
                data['evidence'].append(spoken)
                compact=compact_context(data)
                self.assertEqual(compact['pending_player_message']['event_id'],player['event_id'])
                self.assertFalse(compact['current_player_message']['has_complete_reply'])
                if kind=='SelfSpeech':
                    self.assertIn(spoken['event_id'],compact['recent_self_speech_ids'])
                completed={**spoken,'event_id':str(uuid.uuid4()),'kind':'SelfSpeech',
                           'reply_to_event_id':player['event_id']}
                data['evidence'].append(completed)
                compact=compact_context(data)
                self.assertIsNone(compact['pending_player_message'])
                self.assertEqual(compact['last_complete_player_reply_id'],completed['event_id'])
                self.assertEqual(compact['new_event_ids'],[])

    def test_compaction_keeps_old_target_evidence_and_spoken_words(self):
        data=request()
        player=data['evidence'][0]
        target={'event_id':str(uuid.uuid4()),'subject_id':str(uuid.uuid4()),'kind':'Seen',
                'description':'车停在身旁'}
        reply={'event_id':str(uuid.uuid4()),'subject_id':'','kind':'SelfSpeech',
               'description':'车先挪一下。','reply_to_event_id':player['event_id']}
        data['evidence'].extend([target,reply])
        data['evidence'].extend({'event_id':str(uuid.uuid4()),'subject_id':str(uuid.uuid4()),
                                'kind':'Seen','description':'附近有行人'} for _ in range(14))
        data['candidates'].append({'action':'LookAt','target_id':target['subject_id'],'duration':1,
                                   'evidence_ids':[target['event_id']]})
        compact=compact_context(data)
        kept={item['event_id'] for item in compact['evidence']}
        self.assertIn(target['event_id'],kept)
        self.assertIn(reply['event_id'],kept)
        self.assertEqual(compact['candidates'],data['candidates'])
        self.assertEqual(compact['new_event_ids'],[item['event_id'] for item in data['evidence'][3:]])
        self.assertTrue(set(compact['new_event_ids']).issubset(kept))
        validate_context({**data,'evidence':compact['evidence']})

    def test_vehicle_after_first_32_candidates_remains_available(self):
        data=request()
        data['allowed_actions']=['Wait','LieDown','LookAt','MoveTo','HoldVehicle']
        data['evidence']=[]
        data['candidates']=[{'action':action,'target_id':'','duration':1,'evidence_ids':[]}
                            for action in ('Wait','LieDown')]
        # 按游戏的顺序加入 15 名路人，最后加入一辆可拽住的车
        for index in range(16):
            target,event=str(uuid.uuid4()),str(uuid.uuid4())
            data['evidence'].append({'event_id':event,'subject_id':target,'kind':'Seen',
                                     'description':'看见附近角色' if index<15 else '看见车尾停在身旁'})
            actions=('LookAt','MoveTo') if index<15 else ('LookAt','MoveTo','HoldVehicle')
            data['candidates'].extend({'action':action,'target_id':target,'duration':1,'evidence_ids':[event]}
                                      for action in actions)
        validate_context(data)
        compact=compact_context(data)
        self.assertEqual(len(compact['candidates']),35)
        self.assertEqual(compact['candidates'][34]['action'],'HoldVehicle')
        self.assertTrue(any(item['event_id']==event for item in compact['evidence']))
        events=[];parser=SentenceParser(data,validate_action,lambda kind,value:events.append((kind,value)))
        parser.feed('{"candidate_index":34,"speech":"先把话说清楚。","emotion":"Angry"}')
        parser.finish()
        proposal=events[0][1]
        self.assertEqual(proposal['action'],'HoldVehicle')
        self.assertEqual(proposal['target_id'],target)
        self.assertEqual(proposal['evidence_ids'],[event])

class SentenceTests(unittest.TestCase):
    def test_silent_action_finishes_without_generating_a_sentence(self):
        for action in ('Wait','LookAt','ReleaseVehicle'):
            with self.subTest(action=action):
                data=request()
                evidence=data['evidence'][0]
                data['allowed_actions']=[action]
                data['candidates']=[{'action':action,'duration':1,
                                     'target_id':evidence['subject_id'] if action=='LookAt' else '',
                                     'evidence_ids':[evidence['event_id']] if action=='LookAt' else []}]
                events=[]
                parser=SentenceParser(data,validate_action,lambda kind,value:events.append((kind,value)))
                parser.feed('{"candidate_index":0,"speech":"","emotion":"Calm"}')
                parser.finish()
                self.assertEqual([kind for kind,_ in events],['proposal'])
                self.assertEqual(events[0][1]['action'],action)
                self.assertEqual(events[0][1]['speech'],'')
                self.assertEqual(parser.count,0)

    def test_only_complete_sentence_emits(self):
        events=[];parser=SentenceParser(request(),validate_action,lambda kind,data:events.append((kind,data)))
        parser.feed('{"candidate_index":0,"speech":"你少')
        self.assertEqual(events,[])
        parser.feed('挑事。","emotion":"Calm"}\n{"speech":"我没说他。"')
        self.assertEqual([e[0] for e in events],['proposal','sentence'])
        parser.feed('}')
        parser.finish()
        self.assertEqual(events[-1][1]['sequence'],1)

    def test_complete_object_starts_speech_before_newline_or_turn_end(self):
        events=[];parser=SentenceParser(request(),validate_action,lambda kind,data:events.append((kind,data)))
        parser.feed('{"candidate_index":0,"speech":"你说的}是谁？","emotion":"Calm"}')
        self.assertEqual([event[0] for event in events],['proposal','sentence'])
        self.assertEqual(events[-1][1]['text'],'你说的}是谁？')
        parser.feed('\n{"speech":"我刚才说的是车。"}')
        self.assertEqual(events[-1][1]['sequence'],1)
        parser.finish()

    def test_cancelled_speech_does_not_start_api_request(self):
        cancelled=threading.Event();cancelled.set()
        with patch.dict('os.environ',{'DELIVERY_TTS_URL':'http://unused'}), patch('tts.urllib.request.urlopen') as open_url:
            with self.assertRaises(RuntimeError):
                SpeechSynthesizer().synthesize('我听见了。','male','Calm',cancelled)
            open_url.assert_not_called()

    def test_rejects_new_action_in_later_sentence(self):
        parser=SentenceParser(request(),validate_action,lambda *args:None)
        parser.feed('{"candidate_index":0,"speech":"知道了。","emotion":"Calm"}\n')
        with self.assertRaises(ValueError):parser.feed('{"speech":"现在打他","candidate_index":0}\n')

    def test_rejects_unlisted_action(self):
        parser=SentenceParser(request(),validate_action,lambda *args:None)
        with self.assertRaises(ValueError):parser.feed('{"candidate_index":2,"speech":"回答","emotion":"Calm"}\n')

class SchedulerTests(unittest.TestCase):
    def test_player_starts_while_chatter_is_running(self):
        first_started=threading.Event();player_started=threading.Event();release=threading.Event();order=[]
        def run(job):
            label=job.context['label'];order.append(label)
            if label=='first':first_started.set();release.wait(2)
            elif label=='player':player_started.set()
        scheduler=Scheduler(run,workers=2)
        try:
            scheduler.submit({**request(),'label':'first'})
            self.assertTrue(first_started.wait(1))
            scheduler.submit({**request(),'label':'second'})
            scheduler.submit({**request(),'request_source':'player','label':'player'})
            self.assertTrue(player_started.wait(1))
            self.assertFalse(release.is_set())
            self.assertEqual(order,['first','player'])
        finally:release.set();scheduler.close()

    def test_player_goes_before_waiting_chatter_and_cancel_keeps_slot(self):
        started=threading.Event();release=threading.Event();order=[]
        def run(job):
            order.append(job.context['label'])
            if job.context['label']=='running':started.set();release.wait(2)
        scheduler=Scheduler(run,workers=1)
        try:
            current=scheduler.submit({**request(),'label':'running'})
            self.assertTrue(started.wait(1))
            ambient=scheduler.submit({**request(),'label':'ambient'})
            player=scheduler.submit({**request(),'request_source':'player','label':'player'})
            scheduler.cancel(current.key)
            time.sleep(.03)
            self.assertEqual(order,['running'])
            self.assertFalse(current.finished)
            release.set()
            deadline=time.monotonic()+2
            while not ambient.finished and time.monotonic()<deadline:time.sleep(.01)
            self.assertEqual(order,['running','player','ambient'])
            self.assertTrue(player.finished)
        finally:release.set();scheduler.close()

class ModelConnectionTests(unittest.TestCase):
    def test_each_thread_selects_model_without_replacing_the_process(self):
        service=AppServer()
        process=Mock();process.poll.return_value=None;process.stdin=io.StringIO()
        service.process=process;service.folder=SimpleNamespace(name='npc-test-folder');service.events=queue.Queue()
        for first_id,thread_id,turn_id in ((1,'player-thread','player-turn'),(4,'ambient-thread','ambient-turn')):
            service.events.put({'id':first_id,'result':{'thread':{'id':thread_id}}})
            service.events.put({'id':first_id+1,'result':{'turn':{'id':turn_id}}})
            service.events.put({'method':'item/agentMessage/delta','params':{'threadId':thread_id,'delta':'reply'}})
            service.events.put({'method':'turn/completed','params':{'threadId':thread_id,'turn':{'id':turn_id,'status':'completed'}}})
        replies=[]
        with patch.dict('os.environ',{'DELIVERY_CODEX_MODEL':'configured-ambient'}),patch('app_server.subprocess.Popen') as start:
            service.stream('player prompt','instructions',threading.Event(),replies.append,model='configured-player')
            service.stream('ambient prompt','instructions',threading.Event(),replies.append)
        messages=[json.loads(line) for line in process.stdin.getvalue().splitlines()]
        threads=[item['params'] for item in messages if item['method']=='thread/start']
        self.assertEqual([item['model'] for item in threads],['configured-player','configured-ambient'])
        self.assertTrue(all(item['ephemeral'] and item['approvalPolicy']=='never' and item['sandbox']=='read-only' for item in threads))
        self.assertEqual([item['method'] for item in messages],['thread/start','turn/start','thread/unsubscribe']*2)
        self.assertEqual(replies,['reply','reply'])
        self.assertIs(service.process,process)
        start.assert_not_called()

    def test_cancellation_ends_waiting_for_new_session(self):
        service=AppServer();cancel=threading.Event();sent=threading.Event()
        service.events=queue.Queue()
        service.process=Mock()
        service.process.poll.return_value=None
        service._send=lambda *args:(sent.set() or 1)
        errors=[]
        def wait_for_session():
            try:service._rpc('thread/start',{},cancel)
            except RuntimeError as error:errors.append(str(error))
        thread=threading.Thread(target=wait_for_session,daemon=True);thread.start()
        self.assertTrue(sent.wait(1))
        cancel.set();thread.join(1)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors,['cancelled'])

class ModelSelectionTests(unittest.TestCase):
    def test_token_usage_logs_counts_separately_without_changing_reply(self):
        from test_app_server import token_usage
        class Model:
            last_token_usage = token_usage()
            def stream(self, prompt, instructions, cancel, on_delta, model=None, measure=None):
                on_delta(json.dumps({'candidate_index': 0, 'speech': '', 'emotion': 'Calm'}))
        data = request()
        job = Job(data)
        with patch('streaming._local', SimpleNamespace(model=Model())), patch('builtins.print') as logs:
            run_job(job, 'codex-local', validate_action, None)
        usage = [call.args[0] for call in logs.call_args_list if 'stage=model_token_usage' in call.args[0]]
        self.assertEqual(len(usage), 1)
        self.assertIn('last_input_tokens=120', usage[0])
        self.assertIn('last_cached_input_tokens=80', usage[0])
        self.assertIn('last_reasoning_output_tokens=10', usage[0])
        self.assertIn('thread_total_input_tokens=220', usage[0])
        self.assertNotIn('_ms=', usage[0])
        self.assertEqual(job.events.get_nowait()['type'], 'proposal')
        self.assertEqual(job.events.get_nowait()['type'], 'done')

    def test_player_and_ambient_models_share_the_existing_worker_connection(self):
        selected=[]
        class Model:
            def stream(self,prompt,instructions,cancel,on_delta,model=None,measure=None):
                selected.append(model)
                on_delta(json.dumps({'candidate_index':0,'speech':'','emotion':'Calm'}))
            def close(self):pass
        prepare_resources(local_speech=False)
        try:
            with patch('streaming._local',threading.local()),patch('streaming.AppServer',side_effect=Model) as factory,patch('builtins.print') as logs:
                for settings,cases in (
                    ({'DELIVERY_CODEX_MODEL':'','DELIVERY_CODEX_PLAYER_MODEL':''},
                     [('player','gpt-6.1-sol'),('ambient','gpt-6.1-sol'),(None,'gpt-6.1-sol')]),
                    ({'DELIVERY_CODEX_MODEL':'custom-ambient','DELIVERY_CODEX_PLAYER_MODEL':'custom-player'},
                     [('player','custom-player'),('ambient','custom-ambient')])):
                    with patch.dict('os.environ',settings):
                        for source,expected in cases:
                            data=request();target=data['evidence'][0]
                            data['candidates']=[{'action':'LookAt','target_id':target['subject_id'],'duration':1,'evidence_ids':[target['event_id']]}]
                            if source is not None:data['request_source']=source
                            job=Job(data)
                            run_job(job,'codex-local',validate_action,None)
                            self.assertEqual(selected[-1],expected)
                            self.assertEqual(job.events.get_nowait()['type'],'proposal')
                            self.assertEqual(job.events.get_nowait(),{'type':'done','request_id':job.key[2],'sentences':0})
                            self.assertTrue(job.events.empty())
                self.assertEqual(factory.call_count,1)
                choices=[call.args[0] for call in logs.call_args_list if 'stage=model_selected' in call.args[0]]
                self.assertEqual(len(choices),5)
                self.assertIn('source=player model="gpt-6.1-sol"',choices[0])
                self.assertIn('source=ambient model="gpt-6.1-sol"',choices[1])
        finally:close_resources()

    def test_external_llm_keeps_its_existing_provider(self):
        data=request();target=data['evidence'][0]
        data['request_source']='player'
        data['candidates']=[{'action':'LookAt','target_id':target['subject_id'],'duration':1,'evidence_ids':[target['event_id']]}]
        def external(prompt,cancel,on_delta):
            on_delta(json.dumps({'candidate_index':0,'speech':'','emotion':'Calm'}))
        with patch('streaming.stream_api',side_effect=external) as api,patch('streaming.AppServer') as factory:
            job=Job(data)
            run_job(job,'llm',validate_action,None)
            api.assert_called_once();factory.assert_not_called()
            self.assertEqual(job.events.get_nowait()['type'],'proposal')
            self.assertEqual(job.events.get_nowait()['type'],'done')

class SpeechPreparationTests(unittest.TestCase):
    def test_http_handles_requests_while_voice_is_preparing(self):
        started=threading.Event()
        class PreparingVoice:
            def prepare(self,cancel):started.set();cancel.wait(3)
            def close(self):pass
        with patch('streaming.SpeechSynthesizer',PreparingVoice),patch.dict('os.environ',{'DELIVERY_TTS_URL':''}):
            server=DecisionServer(('127.0.0.1',0),'codex-local')
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                self.assertTrue(started.wait(1))
                url=f'http://127.0.0.1:{server.server_port}/v1/decisions/stream'
                with self.assertRaises(urllib.error.HTTPError) as result:
                    urllib.request.urlopen(urllib.request.Request(url,b'{}'),timeout=1)
                self.assertEqual(result.exception.code,400)
                result.exception.close()
            finally:server.shutdown();server.server_close();thread.join(2)

    def test_close_stops_preparation_and_prevents_new_processes(self):
        started=threading.Event();created=[];lock=threading.Lock()
        class PreparingVoice:
            def __init__(self):
                self.closed=False
                with lock:created.append(self)
            def prepare(self,cancel):
                with lock:
                    if len(created)==2:started.set()
                cancel.wait(2)
            def close(self):self.closed=True
        with patch('streaming.SpeechSynthesizer',PreparingVoice),patch.dict('os.environ',{'DELIVERY_TTS_URL':''}):
            try:
                futures=prepare_resources()
                self.assertTrue(started.wait(1))
                close_resources()
                self.assertEqual(len(created),2)
                self.assertTrue(all(voice.closed for voice in created))
                self.assertTrue(all(future.done() for future in futures))
                with self.assertRaises(RuntimeError):speech_synthesizer()
                self.assertEqual(len(created),2)
            finally:close_resources()

    def test_external_api_is_not_called_during_preparation(self):
        with patch.dict('os.environ',{'DELIVERY_TTS_URL':'http://unused'}),patch('tts.urllib.request.urlopen') as open_url:
            self.assertEqual(prepare_resources(),[])
            self.assertFalse(SpeechSynthesizer().prepare(threading.Event()))
            open_url.assert_not_called()

    def test_closed_voice_does_not_restart(self):
        voice=SpeechSynthesizer();voice.close()
        with patch('tts.subprocess.Popen') as start:
            with self.assertRaises(RuntimeError):voice.prepare(threading.Event())
            start.assert_not_called()

class HttpStreamTests(unittest.TestCase):
    def test_cancel_discards_prepared_audio_and_closes_response(self):
        server=DecisionServer(('127.0.0.1',0),'fixture')
        release=threading.Event();cancelled=threading.Event()
        def run(job):
            job.emit('sentence',pcm='old-audio')
            job.cancel.set();cancelled.set()
            release.wait(2)
        server.scheduler.run=run
        original_submit=server.scheduler.submit
        def submit(data):
            job=original_submit(data)
            self.assertTrue(cancelled.wait(1))
            return job
        server.scheduler.submit=submit
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            url=f'http://127.0.0.1:{server.server_port}/v1/decisions/stream'
            with urllib.request.urlopen(urllib.request.Request(url,json.dumps(request()).encode(),{'Content-Type':'application/json'}),timeout=1) as response:
                self.assertEqual(response.read(),b'')
                self.assertFalse(release.is_set())
        finally:release.set();server.shutdown();server.server_close();thread.join(2)

    def test_first_event_arrives_before_work_finishes(self):
        server=DecisionServer(('127.0.0.1',0),'fixture')
        release=threading.Event()
        def run(job):
            job.emit('proposal',proposal={'action':'Wait'})
            release.wait(2)
            job.emit('done',sentences=0)
        server.scheduler.run=run
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            url=f'http://127.0.0.1:{server.server_port}/v1/decisions/stream'
            with urllib.request.urlopen(urllib.request.Request(url,json.dumps(request()).encode(),{'Content-Type':'application/json'}),timeout=3) as response:
                self.assertEqual(json.loads(response.readline())['type'],'proposal')
                self.assertFalse(release.is_set())
                release.set()
                events=[json.loads(line) for line in response]
                self.assertEqual(events[-1]['type'],'done')
        finally:release.set();server.shutdown();server.server_close();thread.join(2)

if __name__=='__main__':unittest.main()
