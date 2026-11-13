"""检查试听不会覆盖中断文件，也不会把超时或截尾音频当作完成结果"""
import io
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from audition_voice_design import complete_generation
from prepare_voice_design import download_file


class VoiceDesignTests(unittest.TestCase):
    def test_resume_requires_exact_remaining_range(self):
        with tempfile.TemporaryDirectory() as directory:
            partial = Path(directory) / 'model.partial'
            partial.write_bytes(b'first')
            response = io.BytesIO(b'wrong response')
            response.status, response.headers = 200, {}
            with patch('urllib.request.urlopen', return_value=response):
                with self.assertRaisesRegex(ValueError, '后续字节'):
                    download_file({'url': 'https://example.invalid/model', 'bytes': 10, 'path': 'model'}, partial)
            self.assertEqual(partial.read_bytes(), b'first')

    def test_incomplete_generation_is_rejected_and_original_method_restored(self):
        class Sequence:
            def __eq__(self, other):
                return SimpleNamespace(any=lambda: SimpleNamespace(item=lambda: False))
        class Talker:
            def generate(self, **kwargs):
                return SimpleNamespace(sequences=Sequence())
        talker = Talker()
        model = SimpleNamespace(model=SimpleNamespace(
            talker=talker, config=SimpleNamespace(talker_config=SimpleNamespace(codec_eos_token_id=1))))
        with self.assertRaisesRegex(RuntimeError, '台词未正常结束'):
            with complete_generation(model, object, list, time.monotonic() + 1):
                talker.generate()
        self.assertNotIn('generate', vars(talker))

    def test_generation_deadline_stops_and_restores_method(self):
        class Talker:
            def generate(self, **kwargs):
                kwargs['stopping_criteria'][-1](None, None)
                raise AssertionError('deadline ignored')
        talker = Talker()
        model = SimpleNamespace(model=SimpleNamespace(talker=talker))
        with self.assertRaisesRegex(TimeoutError, '90 秒'):
            with complete_generation(model, object, list, time.monotonic() - 1):
                talker.generate()
        self.assertNotIn('generate', vars(talker))


if __name__ == '__main__':
    unittest.main()
