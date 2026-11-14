"""检查小缓存构建只应用已记录的补丁，不把本机修改混入原生对照"""
import difflib
import hashlib
import json
from pathlib import Path
import unittest

from tts_evaluation.prepare_native_kv import ROOT, VENDOR, patched_source


class NativeKvPreparationTest(unittest.TestCase):
    def test_checked_patch_reproduces_fixed_upstream(self):
        source = ROOT / 'source/src/pipeline-tts.cpp'
        if not source.is_file():
            self.skipTest('本机尚未准备固定版本的原生源码')
        manifest = json.loads((VENDOR / 'source-manifest.json').read_text(encoding='utf-8'))
        original = source.read_bytes().decode('utf-8')
        changed = patched_source(original, manifest['files']['src/pipeline-tts.cpp'])
        recorded = manifest['patches']['kv1024']
        self.assertEqual(hashlib.sha256(changed.encode('utf-8')).hexdigest(), recorded['patched_source_sha256'])
        diff = ''.join(difflib.unified_diff(original.splitlines(True), changed.splitlines(True),
                      fromfile='a/src/pipeline-tts.cpp', tofile='b/src/pipeline-tts.cpp')).encode('utf-8')
        self.assertEqual((VENDOR / recorded['path']).read_bytes(), diff)
        self.assertEqual(hashlib.sha256(diff).hexdigest(), recorded['sha256'])
        self.assertEqual(recorded['capacity'], 1024)
        # 输入检查必须发生在为这句分配声码器与执行预填充之前
        guard = changed.index('tts_engine_admit: context capacity exceeded')
        self.assertLess(guard, changed.index('tts_engine_codec_admit', guard))
        self.assertIn('e->slots.pop_back();\n        return tts_admit_fail(job, QT_STATUS_INVALID_PARAMS);', changed[guard:guard + 350])

    def test_changed_source_is_rejected_before_patch(self):
        with self.assertRaisesRegex(ValueError, 'native_kv_source_changed'):
            patched_source('modified upstream', '0' * 64)

    def test_missing_patch_context_is_rejected(self):
        original = 'a valid hash alone is not the expected source'
        with self.assertRaisesRegex(ValueError, 'native_kv_patch_context_changed'):
            patched_source(original, hashlib.sha256(original.encode()).hexdigest())


if __name__ == '__main__':
    unittest.main()
