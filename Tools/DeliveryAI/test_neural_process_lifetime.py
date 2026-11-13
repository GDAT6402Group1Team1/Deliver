"""用当前 Windows venv 检查退出路径，不加载语音模型或 CUDA"""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from neural_tts import NeuralTtsError, QwenSpeechSynthesizer, load_voice_config, project_path


FIXTURE = r'''
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import sys
import time

folder = Path(sys.argv[1])
settings = json.loads((folder / 'fixture.json').read_text(encoding='utf-8'))
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.GetCurrentProcess.restype = wintypes.HANDLE
kernel.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
kernel.IsProcessInJob.restype = wintypes.BOOL
in_job = wintypes.BOOL()
if not kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, ctypes.byref(in_job)):
    raise ctypes.WinError(ctypes.get_last_error())
(folder / 'worker.json').write_text(json.dumps({
    'pid': os.getpid(), 'parent_pid': os.getppid(), 'in_job': bool(in_job.value),
    'executable': sys.executable, 'base_executable': sys._base_executable,
}), encoding='utf-8')
if settings['mode'] == 'startup_timeout':
    time.sleep(60)
else:
    print(json.dumps(settings['ready']), flush=True)
    for line in sys.stdin:
        command = json.loads(line)
        if command['type'] == 'close' and settings['mode'] == 'graceful':
            (folder / 'closed.txt').write_text('closed', encoding='utf-8')
            break
        # 模拟生成卡住，故意不确认取消，检查后端的强制退出路径
        if command['type'] == 'synthesize':
            (folder / 'started.txt').write_text('started', encoding='utf-8')
'''


@unittest.skipUnless(os.name == 'nt', 'Windows venv 进程检查')
class NeuralProcessLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = project_path(load_voice_config()['python_path'])
        if not cls.executable.is_file():
            raise unittest.SkipTest('本机未安装语音试验 venv')
        cls.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        cls.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        cls.kernel.OpenProcess.restype = wintypes.HANDLE
        cls.kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        cls.kernel.WaitForSingleObject.restype = wintypes.DWORD
        cls.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        cls.kernel.CloseHandle.restype = wintypes.BOOL

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='delivery-tts-process-')
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.client = QwenSpeechSynthesizer()
        self.client.config.update(startup_timeout_seconds=2, synthesis_timeout_seconds=.1,
                                  cancel_grace_seconds=.1, process_exit_timeout_seconds=3)
        self.addCleanup(self.client.close)
        # 只启动本测试的轻量脚本，生产 worker 路径不会执行
        original_popen = subprocess.Popen
        self.original_popen = original_popen

        def launch_fixture(*args, **kwargs):
            return original_popen(
                [str(self.executable), '-B', '-X', 'utf8', '-u', '-c', FIXTURE, str(self.folder)],
                **kwargs)

        replacement = patch('neural_tts.subprocess.Popen', side_effect=launch_fixture)
        replacement.start()
        self.addCleanup(replacement.stop)

    def start(self, mode):
        settings = {'mode': mode, 'ready': {
            'type': 'ready', 'protocol_version': 1, 'voice_name': 'Uncle_Fu',
            'sample_rate': 24000, 'voices': self.client.speakers,
            'voice_identities': self.client.identities,
        }}
        (self.folder / 'fixture.json').write_text(json.dumps(settings), encoding='utf-8')
        self.client._start()
        self.wrapper = self.client.process
        deadline = time.monotonic() + 3
        record = self.folder / 'worker.json'
        while not record.is_file() and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertTrue(record.is_file(), '轻量 worker 未启动')
        self.worker = json.loads(record.read_text(encoding='utf-8'))
        self.assertNotEqual(self.wrapper.pid, self.worker['pid'], '本检查需要实际 venv 启动器和解释器两层进程')
        self.assertEqual(self.wrapper.pid, self.worker['parent_pid'])
        # 持有已确认进程的句柄，后面不按 PID 查找或终止其他进程
        self.worker_handle = self.kernel.OpenProcess(0x00100000, False, self.worker['pid'])
        self.assertTrue(self.worker_handle, ctypes.get_last_error())
        self.addCleanup(self.kernel.CloseHandle, self.worker_handle)
        self.assertEqual(self.kernel.WaitForSingleObject(self.worker_handle, 0), 258)
        self.assertTrue(self.worker['in_job'])

    def assert_worker_stopped(self):
        self.assertIsNotNone(self.wrapper.poll())
        self.assertEqual(self.kernel.WaitForSingleObject(self.worker_handle, 3000), 0,
                         'venv 已退出，但实际解释器仍在运行')
        self.assertIsNone(self.client.process)
        self.client.close()
        self.client.close()

    def test_graceful_close_exits_wrapper_and_interpreter(self):
        self.start('graceful')
        self.client.prepare(threading.Event())
        self.client.close()
        self.assert_worker_stopped()
        self.assertTrue((self.folder / 'closed.txt').is_file())

    def test_startup_timeout_exits_wrapper_and_interpreter(self):
        self.start('startup_timeout')
        with self.assertRaisesRegex(NeuralTtsError, 'neural_tts_prepare_timeout'):
            self.client.prepare(threading.Event())
        self.assert_worker_stopped()

    def test_generation_timeout_exits_wrapper_and_interpreter(self):
        # 同时保留一个无关进程，确认超时退出只影响本次语音进程
        unrelated = self.original_popen(
            [sys.executable, '-B', '-c', 'import time; time.sleep(60)'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            self.start('ignore_cancel')
            with self.assertRaisesRegex(NeuralTtsError, 'neural_tts_timeout'):
                self.client.synthesize('这句话不合成声音', 'male', 'Calm', threading.Event())
            self.assert_worker_stopped()
            self.assertTrue((self.folder / 'started.txt').is_file())
            self.assertIsNone(unrelated.poll())
        finally:
            if unrelated.poll() is None:
                unrelated.kill()
            unrelated.wait(timeout=3)

    def test_forced_close_exits_wrapper_and_interpreter(self):
        self.start('ignore_close')
        self.client.prepare(threading.Event())
        self.client.close()
        self.assert_worker_stopped()


if __name__ == '__main__':
    unittest.main()
