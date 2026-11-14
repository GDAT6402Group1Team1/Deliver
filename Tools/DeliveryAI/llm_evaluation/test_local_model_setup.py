"""检查独立模型的下载预算和启动选项，不执行模型"""
import json
from pathlib import Path
import unittest

from prepare_local_model import CONFIG, inside
from start_local_model import cpu_thread_settings, server_arguments


class LocalModelSetupTests(unittest.TestCase):
    def setUp(self):
        self.config = json.loads(CONFIG.read_text(encoding='utf-8'))

    def test_download_is_pinned_and_within_budget(self):
        self.assertLess(sum(row['bytes'] for row in self.config['files']), 2_000_000_000)
        for row in self.config['files']:
            self.assertRegex(row['sha256'], r'^[a-f0-9]{64}$')
            self.assertNotIn('/main/', row['url'])
            self.assertNotIn('/latest/', row['url'])

    def test_cpu_run_keeps_model_and_kv_off_gpu(self):
        args = server_arguments(self.config, Path('llama-server.exe'), 'cpu')
        self.assertEqual(args[args.index('--device') + 1], 'none')
        self.assertEqual(args[args.index('--n-gpu-layers') + 1], '0')
        self.assertIn('--no-kv-offload', args)

    def test_optional_cpu_threads_keep_defaults_and_reject_invalid_values(self):
        default = server_arguments(self.config, Path('llama-server.exe'), 'cpu')
        self.assertEqual(default[default.index('--threads') + 1], '4')
        self.assertEqual(default[default.index('--threads-batch') + 1], '8')
        selected = server_arguments(self.config, Path('llama-server.exe'), 'cpu', threads=8, threads_batch=12)
        self.assertEqual(selected[selected.index('--threads') + 1], '8')
        self.assertEqual(selected[selected.index('--threads-batch') + 1], '12')
        self.assertIn('--no-kv-offload', selected)
        for name in ('threads', 'threads_batch'):
            for value in (0, -1, 65, True, 1.5, '8'):
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    server_arguments(self.config, Path('llama-server.exe'), 'cpu', **{name: value})

    def test_nonthinking_context_and_binding_are_explicit(self):
        args = server_arguments(self.config, Path('llama-server.exe'), 'cuda')
        self.assertEqual(args[args.index('--reasoning') + 1], 'off')
        self.assertEqual(args[args.index('--host') + 1], '127.0.0.1')
        self.assertEqual(args[args.index('--parallel') + 1], '1')
        self.assertEqual(args[args.index('--ctx-size') + 1], '4096')
        self.assertIn('--no-context-shift', args)

    def test_formal_thread_settings_keep_defaults_and_can_select_one_thread(self):
        self.assertEqual(cpu_thread_settings({}), (4, 8))
        self.assertEqual(cpu_thread_settings({}, threads=8, threads_batch=12), (8, 12))
        self.assertEqual(cpu_thread_settings({'threads': 1, 'threads_batch': 1}), (1, 1))
        self.assertEqual(cpu_thread_settings({'threads': 1}, threads_batch=12), (1, 12))
        for name in ('threads', 'threads_batch'):
            for value in (None, True, 0, 65, '1', 1.5):
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    cpu_thread_settings({name: value})

    def test_one_thread_candidate_changes_only_thread_arguments(self):
        original = server_arguments(self.config, Path('llama-server.exe'), 'cuda')
        candidate = server_arguments(self.config, Path('llama-server.exe'), 'cuda', threads=1, threads_batch=1)
        expected = list(original)
        for flag in ('--threads', '--threads-batch'):
            expected[expected.index(flag) + 1] = '1'
        self.assertEqual(candidate, expected)
        self.assertNotIn('--poll', candidate)

    def test_paths_cannot_leave_selected_install_directory(self):
        with self.assertRaises(ValueError):
            inside(Path.cwd(), '../outside-model-file')


if __name__ == '__main__':
    unittest.main()
