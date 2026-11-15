import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import start_local_model as launcher


class StartLocalModelTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        research = self.root / 'Saved/AIResearch'
        self.install = research / 'LocalNpcModel/qwen3-1.7b'
        self.install.mkdir(parents=True)
        self.model = research / 'NpcTraining/combat.gguf'
        self.model.parent.mkdir()
        self.model.write_bytes(b'trained checkpoint')
        self.executable = self.install / 'llama-server.exe'
        self.executable.write_bytes(b'test executable')
        self.config = self.root / 'qwen_local.json'
        self.config.write_text(json.dumps({'host': '127.0.0.1', 'port': 8771, 'context_tokens': 4096,
            'parallel_slots': 1, 'output_tokens': 64, 'runtime_release': 'b10000', 'runtime_commit': 'pinned',
            'files': [{'path': 'unused.gguf', 'sha256': ''}]}), encoding='utf-8')
        self.runtime_files = [{'path': self.executable.name, 'bytes': self.executable.stat().st_size,
                               'sha256': launcher.file_hash(self.executable)}]
        self.write_installation()
        self.manifest = {'schema_version': 1, 'enabled': True, 'release_stage': 'playtest', 'device': 'cuda',
                         'gpu_layers': 18, 'cpu_output_head': True, 'context_tokens': 4096, 'parallel_slots': 2,
                         'output_tokens': 64, 'cache_type': 'f16', 'disable_host_cache': True,
                         'resident_cache': True, 'compact_output': True, 'threads': 1, 'threads_batch': 1,
                         'host': '127.0.0.1', 'port': 18881, 'timeout_seconds': 1.5, 'temperature': 0,
                         'profile_ids': ['chunlai', 'xiumei'], 'model_path': str(self.model.relative_to(self.root)),
                         'model_sha256': launcher.file_hash(self.model)}
        self.manifest_path = self.root / 'combat.json'
        self.process = SimpleNamespace(pid=91, poll=lambda: None)

    def write_installation(self):
        (self.install / 'installation.json').write_text(json.dumps({
            'config_sha256': launcher.file_hash(self.config), 'server_executable': self.executable.name,
            'runtime_files': self.runtime_files}), encoding='utf-8')

    def launch(self, *args):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding='utf-8')
        with patch.multiple(launcher, ROOT=self.root, INSTALL=self.install, CONFIG=self.config), \
                patch.object(sys, 'argv', ['start_local_model.py', '--combat-config', str(self.manifest_path), *args]), \
                contextlib.redirect_stdout(io.StringIO()) as captured:
            launcher.main()
        return [json.loads(line) for line in captured.getvalue().splitlines()]

    def test_playtest_plan_preserves_slots_and_uses_cpu_output_head(self):
        with patch.object(launcher.subprocess, 'Popen') as popen:
            plan, = self.launch()
        popen.assert_not_called()
        self.assertEqual(plan['release_stage'], 'playtest')
        command = plan['command']
        for flag, value in {'--port': '18881', '--n-gpu-layers': '18', '--ctx-size': '8192', '--parallel': '2',
                            '--threads': '1', '--threads-batch': '1', '--cache-type-k': 'f16', '--cache-type-v': 'f16',
                            '--cache-ram': '0', '--override-tensor': launcher.OUTPUT_HEAD_ON_CPU}.items():
            self.assertEqual(command[command.index(flag) + 1], value)
        self.assertIn('--no-cache-idle-slots', command)

    def test_playtest_rejects_inconsistent_manifest(self):
        for key, wrong in [('port', 8771), ('parallel_slots', 1), ('resident_cache', False), ('compact_output', False),
                           ('cpu_output_head', False), ('timeout_seconds', 2), ('profile_ids', ['chunlai', 'companion']),
                           ('model_sha256', '0' * 64)]:
            with self.subTest(key=key), patch.dict(self.manifest, {key: wrong}), self.assertRaises(ValueError):
                self.launch()

    def test_legacy_manifest_keeps_validation_path(self):
        del self.manifest['release_stage']
        self.manifest.update(gpu_layers=21, parallel_slots=1)
        evidence = self.root / 'evaluation.json'
        evidence.write_text('{"summary":{"acceptance_qualified":false}}', encoding='utf-8')
        self.manifest.update(evaluation_path=evidence.name, evaluation_sha256=launcher.file_hash(evidence))
        with self.assertRaisesRegex(ValueError, '模型未通过独立动作验收'):
            self.launch()

    def test_run_records_only_verified_playtest_service(self):
        def reply(url, **kwargs):
            data = {'status': 'ok'} if url.endswith('/health') else [{'id': 0, 'n_ctx': 4096}, {'id': 1, 'n_ctx': 4096}]
            return io.StringIO(json.dumps(data))

        with patch.object(launcher.subprocess, 'Popen', return_value=self.process), \
                patch.object(launcher.subprocess, 'CREATE_NO_WINDOW', 0, create=True), \
                patch.object(launcher, 'existing_process', return_value=None) as reuse, \
                patch.object(launcher.socket, 'socket') as socket_type, patch.object(launcher.urllib.request, 'urlopen', reply):
            socket_type.return_value.__enter__.return_value.connect_ex.return_value = 1
            _, result = self.launch('--run')
        self.assertEqual(reuse.call_args.kwargs, {'slot_count': 2, 'slot_context_tokens': 4096})
        self.assertEqual(result['context_tokens_per_slot'], 4096)
        self.assertEqual(result['model_runtime_release'], 'b10000')
        self.assertTrue((self.install / 'running-playtest.json').is_file())
        self.assertFalse((self.install / 'running.json').exists())

    def test_run_rejects_missing_executable_inventory(self):
        self.runtime_files.clear()
        self.write_installation()
        with patch.object(launcher.subprocess, 'Popen') as popen, self.assertRaisesRegex(ValueError, '安装校验记录'):
            self.launch('--run')
        popen.assert_not_called()

    def test_reuse_requires_same_endpoint_and_both_actual_slot_sizes(self):
        plan, = self.launch()
        command = plan['command']
        record = self.install / 'running-playtest.json'
        record_values = {'pid': 91, 'command': command, 'model_sha256': self.manifest['model_sha256'],
                         'health': 'http://127.0.0.1:18881/health', 'slots': 'http://127.0.0.1:18881/slots'}
        actual_process = SimpleNamespace(returncode=0, stdout=json.dumps({
            'CommandLine': subprocess.list2cmdline(command), 'ExecutablePath': str(self.executable)}))
        record.write_text(json.dumps(record_values), encoding='utf-8')
        with patch.object(launcher, 'os', SimpleNamespace(name='nt')), \
                patch.object(launcher.subprocess, 'run', return_value=actual_process), \
                patch.object(launcher.subprocess, 'CREATE_NO_WINDOW', 0, create=True), \
                patch.object(launcher.urllib.request, 'urlopen') as api:
            for second_size in (2048, 4096):
                api.side_effect = [io.StringIO('{"status":"ok"}'), io.StringIO(json.dumps([
                    {'id': 0, 'n_ctx': 4096}, {'id': 1, 'n_ctx': second_size}]))]
                result = launcher.existing_process(record, command, record_values['model_sha256'], slot_count=2,
                                                   slot_context_tokens=4096)
                self.assertEqual(result is not None, second_size == 4096)
            api.reset_mock()
            record_values['slots'] = 'http://127.0.0.1:9999/slots'
            record.write_text(json.dumps(record_values), encoding='utf-8')
            self.assertIsNone(launcher.existing_process(record, command, record_values['model_sha256'], slot_count=2))
            api.assert_not_called()


if __name__ == '__main__':
    unittest.main()
