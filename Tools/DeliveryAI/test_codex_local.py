"""本机登录适配的命令隔离、候选边界和失败处理，不在单测消费模型额度"""
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import unittest

from codex_local import choose
from companion_context import companion_instructions, ERRAND_INSTRUCTIONS
from server import SYSTEM_PROMPT, validate_action
from test_companion_context import errand_request


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

    @patch("codex_local.shutil.which", return_value="codex.exe")
    def test_v3_prompt_wrapper_keeps_errand_rules_and_trusted_candidate(self, _):
        context = errand_request()
        selected = next(i for i, item in enumerate(context['candidates']) if item['action'] == 'DeliverPackage')
        instructions = SYSTEM_PROMPT + companion_instructions(context)
        observed = []
        fake_runner = self.runner({'candidate_index': selected, 'speech': '我去送这一单。', 'emotion': 'Calm',
                                   'target_id': 'forged', 'duration': 5, 'evidence_ids': []})

        def run(command, **options):
            observed.append(options['input'])
            return fake_runner(command, **options)

        result = validate_action(context, choose(context, instructions, run))
        self.assertTrue(observed[0].startswith(instructions))
        self.assertIn(ERRAND_INSTRUCTIONS, observed[0])
        sent = json.loads(observed[0].rsplit('\n', 1)[1])
        self.assertEqual(sent['capability_version'], 3)
        self.assertEqual(sent['companion_context'], context['companion_context'])
        self.assertEqual(sent['candidates'], context['candidates'])
        self.assertEqual(result['action'], 'DeliverPackage')
        self.assertEqual(result['duration'], 0)
        self.assertEqual(result['target_id'], '')
        self.assertEqual(result['evidence_ids'], context['candidates'][selected]['evidence_ids'])
        self.assertEqual(result['capability_version'], 3)


if __name__ == "__main__":
    unittest.main()
