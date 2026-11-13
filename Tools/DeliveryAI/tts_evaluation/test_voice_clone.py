"""检查参考音来源、取消和重试，测试不加载模型或使用 CUDA"""
import hashlib
from pathlib import Path
import threading
from types import SimpleNamespace
import unittest

from audition_voice_clone import VoiceCloneCancelled, check_generation, collect_codes, first_code_difference
from prepare_voice_design import plan


class VoiceCloneTests(unittest.TestCase):
    def test_code_difference_distinguishes_main_group_subgroup_and_length(self):
        self.assertIsNone(first_code_difference([[1, 2]], [[1, 2]]))
        self.assertEqual(first_code_difference([[1, 2]], [[1, 3]])['group'], 1)
        self.assertEqual(first_code_difference([[1, 2]], [[3, 2]])['group'], 0)
        self.assertTrue(first_code_difference([[1, 2]], [[1, 2], [3, 4]])['frame_count_changed'])

    def test_code_recording_keeps_result_and_restores_existing_method(self):
        codes = [object()]
        output = (codes, object())
        original = lambda **kwargs: output
        owner = SimpleNamespace(generate=original)
        model = SimpleNamespace(model=owner)
        recorded = {}
        with self.assertRaisesRegex(RuntimeError, 'interrupted'):
            with collect_codes(model, recorded):
                self.assertIs(owner.generate(), output)
                self.assertIs(recorded['codes'], codes)
                raise RuntimeError('interrupted')
        self.assertIs(owner.generate, original)

    def test_reference_is_selected_wav_and_new_download_is_within_budget(self):
        project, _, specification, files = plan('voice_clone.json')
        reference = specification['reference']
        self.assertEqual(hashlib.sha256((project / reference['path']).read_bytes()).hexdigest(), reference['sha256'])
        self.assertEqual(sum(item['bytes'] for item in files if item['method'] == 'download'), 1829352406)

    def test_cancel_restores_original_method_and_next_request_can_complete(self):
        cancel = threading.Event()
        class Sequence:
            def __eq__(self, other):
                return SimpleNamespace(any=lambda: SimpleNamespace(item=lambda: True))
        class Talker:
            calls = 0
            def generate(self, **kwargs):
                self.calls += 1
                if self.calls == 1:
                    cancel.set()
                kwargs['stopping_criteria'][-1](None, None)
                return SimpleNamespace(sequences=Sequence())
        talker = Talker()
        model = SimpleNamespace(model=SimpleNamespace(
            talker=talker, config=SimpleNamespace(talker_config=SimpleNamespace(codec_eos_token_id=1))))
        with self.assertRaises(VoiceCloneCancelled):
            with check_generation(model, object, list, cancel):
                talker.generate()
        self.assertNotIn('generate', vars(talker))
        with check_generation(model, object, list, threading.Event()):
            self.assertIsNotNone(talker.generate())
        self.assertNotIn('generate', vars(talker))
        self.assertEqual(talker.calls, 2)


if __name__ == '__main__':
    unittest.main()
