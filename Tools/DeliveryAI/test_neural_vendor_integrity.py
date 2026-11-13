"""检查语音依赖的 Git 换行规则与原始文件，防止检出后无法启动声音"""
import hashlib
from pathlib import Path
import subprocess
import unittest

from neural_tts import PROJECT_ROOT
from neural_tts_worker import VENDOR_HASHES


class NeuralVendorIntegrityTests(unittest.TestCase):
    def test_checkout_preserves_vendor_bytes(self):
        folder = Path(__file__).resolve().parent / 'vendor' / 'faster_qwen_v032'
        paths = [(folder / name).relative_to(PROJECT_ROOT).as_posix() for name in VENDOR_HASHES]
        result = subprocess.run(
            ['git', '-C', str(PROJECT_ROOT), 'check-attr', '-z', 'text', 'eol', '--', *paths],
            check=True, capture_output=True)
        fields = result.stdout.decode('utf-8').rstrip('\0').split('\0')
        attributes = {(fields[index], fields[index + 1]): fields[index + 2]
                      for index in range(0, len(fields), 3)}
        for name, expected in VENDOR_HASHES.items():
            path = folder / name
            relative = path.relative_to(PROJECT_ROOT).as_posix()
            with self.subTest(file=relative):
                self.assertEqual(attributes.get((relative, 'text')), 'set')
                self.assertEqual(attributes.get((relative, 'eol')), 'lf')
                # 使用实际检出的字节检查，不能把 CRLF 转回 LF 后掩盖问题
                content = path.read_bytes()
                self.assertNotIn(b'\r\n', content)
                self.assertEqual(hashlib.sha256(content).hexdigest(), expected)


if __name__ == '__main__':
    unittest.main()
