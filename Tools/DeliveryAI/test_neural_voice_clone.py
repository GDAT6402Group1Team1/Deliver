"""检查固定音色配置、参考复用和取消，使用替身模型，不加载 CUDA"""
import base64
import copy
from contextlib import nullcontext
import hashlib
import json
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import wave

import numpy as np

import neural_tts_worker as worker
from neural_tts import (NeuralTtsError, PROJECT_ROOT, QwenSpeechSynthesizer, load_voice_config,
                        neural_voice_identities, neural_voice_speakers, uses_neural_voice)
from test_neural_tts import FakeProcess


class FakePredictor:
    def __init__(self):
        self.calls = 0
        self.model = SimpleNamespace(config=object())

    def generate(self, **kwargs):
        self.calls += 1
        return SimpleNamespace(sequences=np.zeros((1, 15), dtype=int))


class FakeTalker:
    def __init__(self):
        self.before_step = lambda: None
        self.code_predictor = FakePredictor()

    def generate(self, **kwargs):
        self.before_step()
        kwargs['stopping_criteria'][-1](None, None)
        self.code_predictor.generate(
            inputs_embeds=SimpleNamespace(shape=(1, 2, 4), dtype='bfloat16', device='cuda:0'),
            max_new_tokens=15, do_sample=True, top_p=1.0, top_k=50, temperature=.9,
            output_hidden_states=True, return_dict_in_generate=True)
        return SimpleNamespace(sequences=np.array([17]))


class FakeBaseModel:
    def __init__(self):
        self.model = SimpleNamespace(tts_model_type='base', talker=FakeTalker(),
                                     config=SimpleNamespace(talker_config=SimpleNamespace(
                                         codec_eos_token_id=17, hidden_size=4, num_code_groups=16)))
        self.prompt = [object()]
        self.create_voice_clone_prompt = Mock(return_value=self.prompt)
        self.calls = []

    def generate_voice_clone(self, **kwargs):
        self.calls.append(kwargs)
        self.model.talker.generate()
        return [np.array([0., .25, -.25], dtype=np.float32)], 24000


class NeuralVoiceCloneTests(unittest.TestCase):
    def setUp(self):
        folder = PROJECT_ROOT/'Saved/AIResearch/FemaleVoiceIntegration'
        folder.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix='cpu-test-', dir=folder)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.config = copy.deepcopy(load_voice_config())
        model = self.root/'model'
        files = []
        for name in ('config.json', 'model.safetensors', 'speech_tokenizer/model.safetensors'):
            path = model/name
            path.parent.mkdir(parents=True, exist_ok=True)
            content = ('fixture:'+name).encode()
            path.write_bytes(content)
            files.append({'path': name, 'bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest()})
        self.manifest = {'status': 'completed', 'model_id': 'Qwen/Qwen3-TTS-12Hz-0.6B-Base',
                         'revision': 'a'*40, 'files': files}
        reference = self.root/'reference.wav'
        with wave.open(str(reference), 'wb') as sound:
            sound.setnchannels(1)
            sound.setsampwidth(2)
            sound.setframerate(24000)
            sound.writeframes(b'\0\0'*2400)
        self.entry = {'provider': 'qwen_base', 'speaker': 'Xiaofang_B', 'model_revision': 'a'*40,
                      'model_path': str(model.relative_to(PROJECT_ROOT)),
                      'model_manifest_path': str((self.root/'model-downloads.json').relative_to(PROJECT_ROOT)),
                      'model_manifest_sha256': '', 'reference_path': str(reference.relative_to(PROJECT_ROOT)),
                      'reference_sha256': hashlib.sha256(reference.read_bytes()).hexdigest(),
                      'reference_text': '大军，你少在这儿瞎嚷嚷。'}
        self.config['voices']['female'] = self.entry
        self.config_path = self.root/'config.json'
        self.save_manifest()
        self.save_config()

    def save_manifest(self):
        path = PROJECT_ROOT/self.entry['model_manifest_path']
        path.write_text(json.dumps(self.manifest), encoding='utf-8')
        self.entry['model_manifest_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()

    def save_config(self):
        self.config_path.write_text(json.dumps(self.config, ensure_ascii=False), encoding='utf-8')

    def client(self):
        self.save_config()
        client = QwenSpeechSynthesizer(self.config_path)
        process = FakeProcess(client)
        client.process, client.ready = process, True
        self.addCleanup(client.close)
        return client, process

    def runtime(self, model):
        torch = SimpleNamespace(bfloat16='bfloat16', manual_seed=Mock(), inference_mode=nullcontext,
                                device=lambda name: name,
                                cuda=SimpleNamespace(manual_seed_all=Mock(), synchronize=Mock()))
        factory = SimpleNamespace(from_pretrained=Mock(return_value=model))
        return SimpleNamespace(torch=torch, np=np, ModelClass=factory,
                               StoppingCriteria=object, StoppingCriteriaList=list)

    def task(self, voice='female'):
        return SimpleNamespace(task_id='a'*32, text='你先听我说。', voice=voice,
                               speaker=neural_voice_speakers(self.config)[voice], cancel=threading.Event())

    def test_config_selects_base_without_changing_male_and_rejects_invalid_fields(self):
        config = load_voice_config(self.config_path)
        self.assertEqual(config['voices']['male'], {'provider': 'qwen', 'speaker': 'Uncle_Fu'})
        self.assertEqual(neural_voice_speakers(config), {'male': 'Uncle_Fu', 'female': 'Xiaofang_B'})
        with patch('neural_tts.load_voice_config', return_value=config), patch.dict('os.environ', {'DELIVERY_TTS_PROVIDER':'qwen'}):
            self.assertTrue(uses_neural_voice('female'))
        for key, bad in (('reference_sha256', 'wrong'), ('model_revision', True),
                         ('reference_text', ''), ('speaker', 'Serena'), ('model_path', '../../outside')):
            with self.subTest(key=key):
                previous = self.entry[key]
                self.entry[key] = bad
                self.save_config()
                with self.assertRaises((NeuralTtsError, ValueError)):
                    load_voice_config(self.config_path)
                self.entry[key] = previous

    def test_predictor_mode_defaults_to_official_and_requires_known_choice(self):
        old_identity = neural_voice_identities(self.config)
        self.assertNotIn('predictor_mode', old_identity['female'])
        for mode in ('official', 'cuda_graph'):
            with self.subTest(mode=mode):
                self.entry['predictor_mode'] = mode
                self.save_config()
                loaded = load_voice_config(self.config_path)
                identity = neural_voice_identities(loaded)
                self.assertEqual(identity['male'], old_identity['male'])
                if mode == 'official':
                    self.assertEqual(identity, old_identity)
                else:
                    self.assertEqual(identity['female'], {**old_identity['female'], 'predictor_mode':'cuda_graph'})
        for mode in ('graph', '', None, True, 1, [], {}):
            with self.subTest(invalid=mode):
                self.entry['predictor_mode'] = mode
                self.save_config()
                with self.assertRaisesRegex(NeuralTtsError, 'invalid_predictor_mode'):
                    load_voice_config(self.config_path)

    def test_official_mode_does_not_capture_graph_and_preserves_sampling(self):
        for mode in (None, 'official'):
            with self.subTest(mode=mode), patch.object(worker, 'capture_predictor_graph') as capture:
                if mode is not None:
                    self.entry['predictor_mode'] = mode
                model = FakeBaseModel()
                engine = worker.QwenCloneEngine(self.config, threading.Event(), self.runtime(model))
                engine.synthesize(self.task(), threading.Event())
                capture.assert_not_called()
                self.assertEqual(model.model.talker.code_predictor.calls, 1)
                self.assertEqual(model.calls[-1], {
                    'text':self.task().text, 'language':'Chinese', 'voice_clone_prompt':model.prompt,
                    'non_streaming_mode':True, 'max_new_tokens':256, 'do_sample':True, 'top_k':50,
                    'top_p':1.0, 'temperature':.9, 'repetition_penalty':1.05,
                    'subtalker_dosample':True, 'subtalker_top_k':50, 'subtalker_top_p':1.0,
                    'subtalker_temperature':.9})

    def test_graph_mode_restores_methods_after_failure_and_cancel_then_reuses_model(self):
        self.entry['predictor_mode'] = 'cuda_graph'
        model = FakeBaseModel()
        talker = model.model.talker
        runtime = self.runtime(model)
        tokens = SimpleNamespace(unsqueeze=lambda dimension: np.zeros((1, 15), dtype=int))
        graph = SimpleNamespace(run=Mock(return_value=tokens))
        with patch.object(worker, 'capture_predictor_graph', return_value=(graph, 4)) as capture:
            engine = worker.QwenCloneEngine(self.config, threading.Event(), runtime)
            first = engine.synthesize(self.task(), threading.Event())
            graph.run.side_effect = RuntimeError('test_graph_failed')
            with self.assertRaisesRegex(RuntimeError, 'test_graph_failed'):
                engine.synthesize(self.task(), threading.Event())
            for owner in (talker, talker.code_predictor):
                self.assertNotIn('generate', vars(owner))
            cancelled = self.task()
            def cancel_after_replay(inputs):
                cancelled.cancel.set()
                return tokens
            graph.run.side_effect = cancel_after_replay
            with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
                engine.synthesize(cancelled, threading.Event())
            for owner in (talker, talker.code_predictor):
                self.assertNotIn('generate', vars(owner))
            graph.run.side_effect = None
            second = engine.synthesize(self.task(), threading.Event())
            self.assertEqual(first['pcm'], second['pcm'])
            self.assertEqual(graph.run.call_count, 4)
            self.assertEqual(talker.code_predictor.calls, 0)
            capture.assert_called_once()
            model.create_voice_clone_prompt.assert_called_once()
            runtime.ModelClass.from_pretrained.assert_called_once()

    def test_graph_capture_uses_fp32_sampling_and_restores_it_on_failure(self):
        model = FakeBaseModel()
        runtime = self.runtime(model)
        logits = SimpleNamespace(float=Mock(return_value=object()))
        original_sample = Mock(return_value='sampled')
        module = SimpleNamespace(
            __file__=str(Path(worker.__file__).parent/'vendor/faster_qwen_v032/predictor_graph.py'),
            sample_logits=original_sample)
        graph = SimpleNamespace(capture=Mock())
        module.PredictorGraph = Mock(return_value=graph)
        def capture(**kwargs):
            self.assertEqual(kwargs, {'num_warmup':3})
            module.sample_logits(logits, temperature=.9)
        graph.capture.side_effect = capture
        with patch.object(worker.importlib, 'import_module', return_value=module):
            result, hidden = worker.capture_predictor_graph(model, runtime.torch, lambda name: None)
            self.assertIs(result, graph)
            self.assertEqual(hidden, 4)
            self.assertIs(module.sample_logits, original_sample)
            original_sample.assert_called_once_with(logits.float.return_value, temperature=.9)
            self.assertEqual(module.PredictorGraph.call_args.kwargs, {
                'device':'cuda:0', 'dtype':'bfloat16', 'do_sample':True,
                'top_k':50, 'top_p':1.0, 'temperature':.9})
            graph.capture.side_effect = RuntimeError('test_capture_failed')
            with self.assertRaisesRegex(RuntimeError, 'test_capture_failed'):
                worker.capture_predictor_graph(model, runtime.torch, lambda name: None)
            self.assertIs(module.sample_logits, original_sample)

    def test_manifest_reference_hash_and_model_boundaries_are_checked(self):
        worker.checked_clone_assets(self.entry)
        reference = PROJECT_ROOT/self.entry['reference_path']
        reference.write_bytes(reference.read_bytes()+b'changed')
        with self.assertRaisesRegex(NeuralTtsError, 'reference_changed'):
            worker.checked_clone_assets(self.entry)
        self.manifest['files'][0]['path'] = '../reference.wav'
        self.save_manifest()
        with self.assertRaisesRegex(NeuralTtsError, 'outside_model'):
            worker.checked_clone_assets(self.entry)
        self.entry['model_manifest_sha256'] = '0'*64
        with self.assertRaisesRegex(NeuralTtsError, 'manifest_changed'):
            worker.checked_clone_assets(self.entry)

    def test_modified_model_is_rejected_before_model_load(self):
        path = PROJECT_ROOT/self.entry['model_path']/'model.safetensors'
        path.write_bytes(b'x'*path.stat().st_size)
        runtime = self.runtime(FakeBaseModel())
        with self.assertRaisesRegex(NeuralTtsError, 'model_changed'):
            worker.QwenCloneEngine(self.config, threading.Event(), runtime)
        runtime.ModelClass.from_pretrained.assert_not_called()

    def test_reference_is_created_once_and_reused_after_cancellation(self):
        model = FakeBaseModel()
        runtime = self.runtime(model)
        engine = worker.QwenCloneEngine(self.config, threading.Event(), runtime)
        first = engine.synthesize(self.task(), threading.Event())
        cancelled = self.task()
        model.model.talker.before_step = cancelled.cancel.set
        with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
            engine.synthesize(cancelled, threading.Event())
        self.assertNotIn('generate', vars(model.model.talker))
        model.model.talker.before_step = lambda: None
        second = engine.synthesize(self.task(), threading.Event())
        runtime.ModelClass.from_pretrained.assert_called_once()
        model.create_voice_clone_prompt.assert_called_once()
        self.assertTrue(all(call['voice_clone_prompt'] is model.prompt for call in model.calls))
        self.assertTrue(all('ref_audio' not in call and 'speaker' not in call for call in model.calls))
        self.assertEqual(first['pcm'], second['pcm'])
        self.assertEqual(first['voice_name'], 'Xiaofang_B')

    def test_two_models_are_created_once_and_male_uses_original_engine(self):
        custom, clone = Mock(), Mock()
        custom.synthesize.side_effect = lambda task, closed: {'voice_name': task.speaker}
        clone.synthesize.side_effect = lambda task, closed: {'voice_name': task.speaker}
        with patch.object(worker, 'QwenEngine', return_value=custom) as custom_factory, \
             patch.object(worker, 'QwenCloneEngine', return_value=clone) as clone_factory:
            engines = worker.QwenVoiceEngines(self.config, threading.Event())
            for voice in ('male', 'female', 'female', 'male'):
                result = engines.synthesize(self.task(voice), threading.Event())
                self.assertEqual(result['voice_identity'], neural_voice_identities(self.config)[voice])
            custom_factory.assert_called_once()
            clone_factory.assert_called_once()
            self.assertEqual(custom.synthesize.call_count, 2)
            self.assertEqual(clone.synthesize.call_count, 2)
            engines.synchronize()
            custom.synchronize.assert_called_once()

    def test_threads_are_initialized_once(self):
        torch = Mock()
        with patch.object(worker, '_torch_threads_initialized', False):
            worker.initialize_torch_threads(torch)
            worker.initialize_torch_threads(torch)
        torch.set_num_threads.assert_called_once_with(4)
        torch.set_num_interop_threads.assert_called_once_with(1)

    def test_ready_and_audio_must_match_fixed_reference_identity(self):
        client, process = self.client()
        client.ready = False
        wrong = copy.deepcopy(client.identities)
        wrong['female']['reference_sha256'] = '0'*64
        client.messages.put({'type':'ready', 'protocol_version':1, 'voice_name':'Uncle_Fu',
                             'sample_rate':24000, 'voices':client.speakers, 'voice_identities':wrong})
        with self.assertRaisesRegex(NeuralTtsError, 'prepare_failed'):
            client.prepare(threading.Event())
        self.assertTrue(process.killed)
        client, process = self.client()
        process.reply = lambda task_id: client.messages.put({
            'type':'audio', 'task_id':task_id, 'voice_name':'Xiaofang_B', 'sample_rate':24000,
            'voice_identity':wrong['female'], 'pcm':base64.b64encode(b'\0\0').decode()})
        with self.assertRaisesRegex(NeuralTtsError, 'invalid_audio_format'):
            client.synthesize('你找谁？', 'female', 'Calm', threading.Event())
        self.assertTrue(process.killed)

    def test_ready_accepts_both_verified_voice_identities(self):
        client, process = self.client()
        client.ready = False
        client.messages.put({'type':'ready', 'protocol_version':1, 'voice_name':'Uncle_Fu',
                             'sample_rate':24000, 'voices':client.speakers, 'voice_identities':client.identities})
        self.assertTrue(client.prepare(threading.Event()))
        self.assertEqual(client.config_path, self.config_path)
        self.assertFalse(process.killed)

    def test_ready_and_audio_reject_other_predictor_mode_in_both_directions(self):
        for mode in ('official', 'cuda_graph'):
            with self.subTest(mode=mode):
                self.entry['predictor_mode'] = mode
                client, process = self.client()
                wrong = copy.deepcopy(client.identities)
                if mode == 'official':
                    wrong['female']['predictor_mode'] = 'cuda_graph'
                else:
                    del wrong['female']['predictor_mode']
                client.ready = False
                client.messages.put({'type':'ready', 'protocol_version':1, 'voice_name':'Uncle_Fu',
                                     'sample_rate':24000, 'voices':client.speakers, 'voice_identities':wrong})
                with self.assertRaisesRegex(NeuralTtsError, 'prepare_failed'):
                    client.prepare(threading.Event())
                self.assertTrue(process.killed)
                client, process = self.client()
                process.reply = lambda task_id: client.messages.put({
                    'type':'audio', 'task_id':task_id, 'voice_name':'Xiaofang_B', 'sample_rate':24000,
                    'voice_identity':wrong['female'], 'pcm':base64.b64encode(b'\0\0').decode()})
                with self.assertRaisesRegex(NeuralTtsError, 'invalid_audio_format'):
                    client.synthesize('你找谁？', 'female', 'Calm', threading.Event())
                self.assertTrue(process.killed)

    def test_graph_identity_is_accepted_by_same_configured_client(self):
        self.entry['predictor_mode'] = 'cuda_graph'
        client, process = self.client()
        client.ready = False
        client.messages.put({'type':'ready', 'protocol_version':1, 'voice_name':'Uncle_Fu',
                             'sample_rate':24000, 'voices':client.speakers, 'voice_identities':client.identities})
        self.assertTrue(client.prepare(threading.Event()))
        self.assertEqual(client.synthesize('你找谁？', 'female', 'Calm', threading.Event())[1], 24000)
        self.assertFalse(process.killed)

    def test_original_male_sampling_arguments_are_unchanged(self):
        runtime = self.runtime(FakeBaseModel())
        generate = Mock(return_value=([np.array([0., .25, -.25], dtype=np.float32)], 24000))
        engine = worker.QwenEngine.__new__(worker.QwenEngine)
        engine.config, engine.torch, engine.np = self.config, runtime.torch, runtime.np
        engine.model = SimpleNamespace(generate_custom_voice=generate)
        task = self.task('male')
        for mode in ('official', 'cuda_graph'):
            self.entry['predictor_mode'] = mode
            with self.subTest(female_mode=mode), \
                 patch.object(worker, 'generation_controls', return_value=nullcontext()) as controls:
                result = engine.synthesize(task, threading.Event())
                controls.assert_called_once()
        self.assertEqual(generate.call_args.kwargs, {
            'text':task.text, 'language':'Chinese', 'speaker':'Uncle_Fu', 'non_streaming_mode':True,
            'max_new_tokens':256, 'do_sample':True, 'top_k':50, 'top_p':1.0, 'temperature':.9,
            'repetition_penalty':1.05, 'subtalker_dosample':True, 'subtalker_top_k':50,
            'subtalker_top_p':1.0, 'subtalker_temperature':.9})
        self.assertEqual(result['voice_name'], 'Uncle_Fu')

    def test_base_voice_uses_same_worker_protocol(self):
        client, process = self.client()
        for voice in ('female', 'male', 'female'):
            self.assertEqual(client.synthesize('你找谁？', voice, 'Calm', threading.Event())[1], 24000)
        self.assertEqual([command['voice'] for command in process.commands], ['female', 'male', 'female'])


if __name__ == '__main__':
    unittest.main()
