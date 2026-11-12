"""检查本机语音宿主的选择，保留 Windows PowerShell 兼容路径"""
import unittest
from unittest.mock import patch
from tts import local_shell


class VoiceShellTests(unittest.TestCase):
    def test_prefers_installed_powershell_7(self):
        with patch.dict('os.environ',{'DELIVERY_TTS_SHELL':''}),patch('tts.shutil.which',side_effect=lambda name:{'pwsh.exe':'pwsh-path','powershell.exe':'legacy-path'}.get(name)):
            self.assertEqual(local_shell(),'pwsh-path')

    def test_falls_back_to_windows_powershell(self):
        with patch.dict('os.environ',{'DELIVERY_TTS_SHELL':''}),patch('tts.shutil.which',side_effect=lambda name:{'powershell.exe':'legacy-path'}.get(name)):
            self.assertEqual(local_shell(),'legacy-path')

    def test_explicit_path_overrides_automatic_selection(self):
        with patch.dict('os.environ',{'DELIVERY_TTS_SHELL':'chosen-path'}),patch('tts.shutil.which',side_effect=lambda name:'chosen-path' if name=='chosen-path' else 'pwsh-path'):
            self.assertEqual(local_shell(),'chosen-path')

    def test_missing_override_reports_configuration_error(self):
        with patch.dict('os.environ',{'DELIVERY_TTS_SHELL':'missing-path'}),patch('tts.shutil.which',return_value=None):
            with self.assertRaisesRegex(RuntimeError,'tts_shell_not_found'):local_shell()


if __name__=='__main__':unittest.main()
