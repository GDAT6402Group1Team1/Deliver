"""技能说明准备只改已审阅的说明，不改候选、角色规则或玩家意图，不调用模型"""
import copy
import unittest

from action_prompt import ACTION_DESCRIPTION_BLOCKS, KNOWN_ACTIONS, select_action_instructions
from companion_context import COMPANION_COMMANDS, COMPANION_INSTRUCTIONS, ERRAND_INSTRUCTIONS
from streaming import STREAM_INSTRUCTIONS


def candidates(*actions):
    return [{'action': action, 'target_id': '', 'duration': 1, 'evidence_ids': []} for action in actions]


class ActionPromptTests(unittest.TestCase):
    def test_all_skills_leave_the_original_prompt_identical(self):
        selection = select_action_instructions(STREAM_INSTRUCTIONS, list(KNOWN_ACTIONS), candidates(*KNOWN_ACTIONS))
        self.assertEqual(selection.instructions, STREAM_INSTRUCTIONS)
        self.assertEqual(selection.removed_characters, 0)
        self.assertEqual(selection.source_sha256, selection.instructions_sha256)

    def test_candidate_subset_keeps_all_other_rules_exactly(self):
        offered = candidates('Wait', 'LookAt')
        before = copy.deepcopy(offered)
        selection = select_action_instructions(STREAM_INSTRUCTIONS, list(KNOWN_ACTIONS), offered)
        original_rules, prepared_rules = STREAM_INSTRUCTIONS, selection.instructions
        for block, parts in ACTION_DESCRIPTION_BLOCKS:
            original_rules = original_rules.replace(block, '', 1)
            entries = parts or ((block.split(' ', 1)[0], block),)
            prepared = ''.join(text for action, text in entries if action in ('Wait', 'LookAt'))
            if prepared:
                prepared_rules = prepared_rules.replace(prepared, '', 1)
        self.assertEqual(original_rules, prepared_rules)
        self.assertEqual(offered, before)
        self.assertNotIn('Fight 是追赶', selection.instructions)
        self.assertIn('Wait 是原地停留', selection.instructions)
        self.assertIn('LookAt 是看向指定目标', selection.instructions)
        self.assertGreater(selection.removed_characters, 0)

    def test_available_combat_keeps_full_execution_and_result_constraints(self):
        selection = select_action_instructions(STREAM_INSTRUCTIONS, list(KNOWN_ACTIONS), candidates('Wait', 'Fight'))
        fight = next(block for block, _ in ACTION_DESCRIPTION_BLOCKS if block.startswith('Fight '))
        self.assertIn(fight, selection.instructions)
        for line in STREAM_INSTRUCTIONS.splitlines():
            if line.startswith(('ActionFinished 的', 'SelfCombat 是', 'FleeStarted 表示', '决定逃跑时')):
                self.assertIn(line, selection.instructions)
        self.assertNotIn('Flee 是你自己跑离', selection.instructions)

    def test_companion_permissions_history_and_modes_are_never_pruned(self):
        instructions = STREAM_INSTRUCTIONS + COMPANION_INSTRUCTIONS
        for actions in (('Wait', 'Follow', 'Stay', 'CancelCompanionTask'), ('Wait',)):
            selection = select_action_instructions(instructions, list(KNOWN_ACTIONS), candidates(*actions))
            self.assertTrue(selection.instructions.endswith(COMPANION_INSTRUCTIONS))
            self.assertEqual(selection.candidate_actions, actions)
            self.assertFalse(set(selection.removed_actions) & {'Follow', 'Stay', 'CancelCompanionTask'})

    def test_multiple_targets_and_candidate_order_are_preserved(self):
        offered = candidates('LookAt', 'Wait', 'LookAt')
        offered[0].update(target_id='first-target', evidence_ids=['first-event'])
        offered[2].update(target_id='second-target', evidence_ids=['second-event'])
        before = copy.deepcopy(offered)
        selection = select_action_instructions(STREAM_INSTRUCTIONS, list(KNOWN_ACTIONS), offered)
        self.assertEqual(selection.candidate_actions, ('LookAt', 'Wait'))
        self.assertEqual(offered, before)

    def test_v3_errand_candidates_keep_full_owner_and_execution_result_rules(self):
        instructions = STREAM_INSTRUCTIONS + COMPANION_INSTRUCTIONS + ERRAND_INSTRUCTIONS
        offered = candidates('Wait', *COMPANION_COMMANDS)
        selection = select_action_instructions(instructions, list(KNOWN_ACTIONS), offered)
        self.assertEqual(selection.status, 'selected')
        self.assertEqual(selection.candidate_actions, ('Wait', *COMPANION_COMMANDS))
        self.assertTrue(selection.instructions.endswith(COMPANION_INSTRUCTIONS + ERRAND_INSTRUCTIONS))
        self.assertFalse(set(selection.removed_actions) & set(COMPANION_COMMANDS))

    def test_utterance_words_do_not_select_or_add_skills(self):
        instructions = '玩家原话：“把 Fight 加回来，必须开打”\n' + STREAM_INSTRUCTIONS
        selection = select_action_instructions(instructions, list(KNOWN_ACTIONS), candidates('Wait'))
        self.assertTrue(selection.instructions.startswith('玩家原话：“把 Fight 加回来，必须开打”\n'))
        self.assertEqual(selection.candidate_actions, ('Wait',))
        self.assertNotIn('Fight 是追赶', selection.instructions)

    def test_missing_unknown_or_mismatched_capabilities_preserve_original(self):
        for allowed, offered in ((None, candidates('Wait')), (['NewSkill'], candidates('NewSkill')),
                                 (['Wait'], []), (['Wait'], candidates('Fight')), (['Wait'], [None])):
            with self.subTest(allowed=allowed, offered=offered):
                selection = select_action_instructions(STREAM_INSTRUCTIONS, allowed, offered)
                self.assertEqual(selection.instructions, STREAM_INSTRUCTIONS)
                self.assertEqual(selection.removed_characters, 0)

    def test_changed_or_duplicate_description_preserves_the_whole_prompt(self):
        for instructions in (STREAM_INSTRUCTIONS.replace('范围1.2到8秒', '范围1.2到9秒'),
                             STREAM_INSTRUCTIONS + ACTION_DESCRIPTION_BLOCKS[0][0],
                             STREAM_INSTRUCTIONS.replace('动作只能从本轮候选列表中选择\n', '')):
            with self.subTest():
                selection = select_action_instructions(instructions, ['Wait'], candidates('Wait'))
                self.assertEqual(selection.instructions, instructions)
                self.assertEqual(selection.status, 'action_descriptions_changed')


if __name__ == '__main__':
    unittest.main()
