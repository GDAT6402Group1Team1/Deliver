"""本机登录适配的命令隔离、候选边界和失败处理，不在单测消费模型额度"""
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import unittest

from codex_local import choose


class LocalCodexTests(unittest.TestCase):
    def request(self):
        return {"candidates": [{"action": "Wait", "target_id": "", "duration": 2, "evidence_ids": []}]}

    def runner(self, response):
        def run(command, **options):
            self.assertIn("--ignore-user-config", command)
            self.assertIn("--ephemeral", command)
            self.assertIn("read-only", command)
            self.assertNotIn("shell", options)
            self.assertEqual(command[-1], "-")
            self.assertEqual(options["timeout"], 24)
            self.assertIn("shell_tool", command)
            Path(command[command.index("--output-last-message") + 1]).write_text(json.dumps(response), encoding="utf8")
            return SimpleNamespace(returncode=0)
        return run

    @patch("codex_local.shutil.which", return_value="codex.exe")
    def test_selects_trusted_tuple(self, _):
        result = choose(self.request(), "测试指令", self.runner({"candidate_index": 0, "speech": "让你先过", "emotion": "Calm", "target_id": "forged"}))
        self.assertEqual(result["target_id"], "")
        self.assertEqual(result["duration"], 2)

    @patch("codex_local.shutil.which", return_value="codex.exe")
    def test_rejects_invalid_index_and_failed_process(self, _):
        for index in (True, -1, 1, "0"):
            with self.subTest(index=index), self.assertRaises(ValueError):
                choose(self.request(), "", self.runner({"candidate_index": index}))
        with self.assertRaises(RuntimeError):
            choose(self.request(), "", lambda *a, **k: SimpleNamespace(returncode=1))


if __name__ == "__main__":
    unittest.main()
