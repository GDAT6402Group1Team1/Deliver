"""检查分段进程协议、取消与异常恢复，不启动模型或 GPU"""
import base64
import queue
import threading
import unittest

from neural_tts import NeuralTtsError, QwenSpeechSynthesizer
from test_neural_tts import FakeProcess


class NeuralStreamTests(unittest.TestCase):
    def client(self):
        client = QwenSpeechSynthesizer()
        process = FakeProcess(client)
        client.process, client.ready, client.audio_stream_version = process, True, 1
        client.config = {**client.config, 'cancel_grace_seconds':1, 'process_exit_timeout_seconds':.1}
        self.addCleanup(client.close)
        return client, process

    def message(self, client, task, kind, **fields):
        return {'type':kind, 'task_id':task, 'voice_name':client.speakers['female'],
                'voice_identity':client.identities['female'], 'sample_rate':24000, **fields}

    def chunk(self, client, task, index=0, offset=0, pcm=b'\x01\x00'*8, **fields):
        return self.message(client, task, 'audio_chunk', **{
            'chunk_index':index, 'offset_bytes':offset, 'pcm':base64.b64encode(pcm).decode(), **fields})

    def end(self, client, task, count=1, total=16):
        return self.message(client, task, 'audio_end', chunk_count=count, total_pcm_bytes=total)

    def stream(self, client, callback, cancel=None):
        return client.synthesize_stream('你先听我说。','female','Calm',cancel or threading.Event(),callback)

    def test_complete_sentence_returns_total_and_does_not_repeat_tail(self):
        client, process = self.client()
        def reply(task):
            client.messages.put(self.chunk(client, task))
            client.messages.put(self.chunk(client, task, 1, 16, b'\x02\x00'))
            client.messages.put(self.end(client, task, 2, 18))
        process.reply = reply
        chunks = []
        self.assertEqual(self.stream(client, chunks.append), (18,24000))
        self.assertEqual(chunks,[b'\x01\x00'*8,b'\x02\x00'])
        self.assertEqual(process.commands[0]['chunk_frames'],8)
        self.assertEqual(client.voice_name,client.speakers['female'])
        self.assertFalse(process.killed)

    def test_old_ready_keeps_full_audio_but_refuses_unnegotiated_stream(self):
        client, process = self.client()
        client.ready=False
        client.messages.put({'type':'ready','protocol_version':1,'voice_name':'Uncle_Fu',
                             'voices':client.speakers,'voice_identities':client.identities,'sample_rate':24000})
        with self.assertRaisesRegex(NeuralTtsError,'stream_unsupported'):
            self.stream(client,lambda pcm: self.fail('old worker cannot stream'))
        self.assertEqual(process.commands,[])
        self.assertFalse(process.killed)
        self.assertEqual(client.synthesize('你好。','female','Calm',threading.Event())[1],24000)
        self.assertNotIn('chunk_frames',process.commands[0])

    def test_ready_capability_and_stream_options_are_checked(self):
        client, process = self.client()
        client.ready=False
        client.messages.put({'type':'ready','protocol_version':1,'voice_name':'Uncle_Fu',
                             'voices':client.speakers,'voice_identities':client.identities,
                             'sample_rate':24000,'audio_stream_version':1})
        client.prepare(threading.Event())
        self.assertTrue(client.supports_audio_stream)
        for frames in (0,True,3,16):
            with self.subTest(frames=frames), self.assertRaisesRegex(NeuralTtsError,'invalid_stream_options'):
                client.synthesize_stream('你好。','female','Calm',threading.Event(),lambda pcm: None,frames)
        self.assertEqual(process.commands,[])

    def test_duplicate_offset_identity_size_and_invalid_end_are_rejected(self):
        cases = ('duplicate','gap','wrong_voice','odd','oversized','bad_base64','wrong_total','no_chunks')
        for case in cases:
            with self.subTest(case=case):
                client, process = self.client()
                def reply(task):
                    if case!='no_chunks': client.messages.put(self.chunk(client,task))
                    if case=='duplicate': client.messages.put(self.chunk(client,task))
                    if case=='gap': client.messages.put(self.chunk(client,task,1,18))
                    if case=='wrong_voice':
                        client.messages.put(self.chunk(client,task,1,16,voice_name='wrong'))
                    if case=='odd': client.messages.put(self.chunk(client,task,1,16,b'\0'))
                    if case=='oversized': client.messages.put(self.chunk(client,task,1,16,b'\0'*32770))
                    if case=='bad_base64':
                        invalid=self.chunk(client,task,1,16)
                        invalid['pcm']='!'
                        client.messages.put(invalid)
                    client.messages.put(self.end(client,task,1,18 if case=='wrong_total' else 16))
                process.reply=reply
                chunks=[]
                with self.assertRaises((NeuralTtsError,ValueError,TypeError)):
                    self.stream(client,chunks.append)
                self.assertLessEqual(len(chunks),1)
                self.assertTrue(process.killed,'协议出错后停止旧进程，避免残留块混入下一句')
                if case not in ('wrong_total','no_chunks'):
                    self.assertTrue(process.cancel_received.is_set())

    def test_total_audio_and_chunk_count_are_bounded(self):
        for count,size in ((13,30720),(33,2)):
            with self.subTest(count=count,size=size):
                client,process=self.client()
                def reply(task):
                    for index in range(count):
                        client.messages.put(self.chunk(client,task,index,index*size,b'\0'*size))
                    client.messages.put(self.end(client,task,count,count*size))
                process.reply=reply
                chunks=[]
                with self.assertRaisesRegex(NeuralTtsError,'invalid_stream_(size|order)'):
                    self.stream(client,chunks.append)
                self.assertLessEqual(sum(map(len,chunks)),384000)
                self.assertLessEqual(len(chunks),32)

    def test_cancel_timeout_and_callback_error_wait_for_worker_to_stop(self):
        for reason in ('cancel','timeout','callback'):
            with self.subTest(reason=reason):
                client,process=self.client()
                process.auto_audio=False
                if reason=='timeout': client.config['synthesis_timeout_seconds']=.06
                cancel=threading.Event()
                delivered=threading.Event()
                errors=[]
                def callback(pcm):
                    delivered.set()
                    if reason=='callback': raise queue.Full('test_backpressure')
                def run():
                    try: self.stream(client,callback,cancel)
                    except Exception as error: errors.append(error)
                thread=threading.Thread(target=run)
                thread.start()
                self.assertTrue(process.started.wait(1))
                task=process.commands[0]['task_id']
                client.messages.put(self.chunk(client,task))
                self.assertTrue(delivered.wait(1))
                if reason=='cancel': cancel.set()
                self.assertTrue(process.cancel_received.wait(1))
                client.messages.put(self.chunk(client,task,1,16))
                thread.join(.05)
                self.assertTrue(thread.is_alive(),'迟到的块不能表示子进程已经停止')
                self.assertTrue(client.lock.locked())
                client.messages.put({'type':'cancelled','task_id':task,'code':'cancelled'})
                thread.join(1)
                self.assertFalse(thread.is_alive())
                self.assertFalse(client.lock.locked())
                self.assertEqual(len(errors),1)
                if reason=='callback': self.assertIsInstance(errors[0],queue.Full)
                else: self.assertEqual(errors[0].code,'cancelled' if reason=='cancel' else 'neural_tts_timeout')
                self.assertFalse(process.killed)
                process.auto_audio=True
                self.assertEqual(client.synthesize('下一句。','male','Calm',threading.Event())[1],24000)

    def test_callback_failure_force_stops_worker_that_never_confirms_cancel(self):
        client,process=self.client()
        client.config['cancel_grace_seconds']=.06
        process.reply=lambda task: client.messages.put(self.chunk(client,task))
        def failed(pcm): raise queue.Full('test_backpressure')
        with self.assertRaises(queue.Full): self.stream(client,failed)
        self.assertTrue(process.killed)
        self.assertIsNone(client.process)

    def test_late_previous_end_cannot_be_used_for_next_sentence(self):
        client,process=self.client()
        def reply(task):
            client.messages.put(self.chunk(client,task))
            client.messages.put(self.end(client,task))
            client.messages.put(self.end(client,task))
        process.reply=reply
        self.stream(client,lambda pcm: None)
        with self.assertRaisesRegex(NeuralTtsError,'unexpected_message'):
            self.stream(client,lambda pcm: self.fail('late data must not play'))
        self.assertTrue(process.killed)


if __name__=='__main__':
    unittest.main()
