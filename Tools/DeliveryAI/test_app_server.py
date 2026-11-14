"""检查打断后的进程复用、迟到消息隔离和计时，不启动模型或外部服务"""
import io
import json
import os
from pathlib import Path
import queue
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

from app_server import AppServer


def notification(method, thread, turn=None, **fields):
    params = {'threadId': thread, **fields}
    if turn is not None:
        params['turnId'] = turn
    return {'method': method, 'params': params}


def terminal(thread, turn, status):
    return {'method': 'turn/completed', 'params': {'threadId': thread, 'turn': {'id': turn, 'status': status}}}


def token_usage():
    return {'last': {'inputTokens': 120, 'cachedInputTokens': 80, 'outputTokens': 25,
                     'reasoningOutputTokens': 10, 'totalTokens': 145},
            'total': {'inputTokens': 220, 'cachedInputTokens': 130, 'outputTokens': 40,
                      'reasoningOutputTokens': 15, 'totalTokens': 260, 'cacheWriteInputTokens': 12}}


class InputPipe(io.StringIO):
    def __init__(self, respond):
        super().__init__()
        self.respond = respond

    def write(self, text):
        result = super().write(text)
        self.respond(json.loads(text))
        return result


class AppServerTests(unittest.TestCase):
    def setUp(self):
        self.service = AppServer()
        self.process = Mock(pid=123456)
        self.process.poll.return_value = None
        self.process.stdin = InputPipe(self.respond)
        self.service.process = self.process
        self.service.folder = SimpleNamespace(
            name=str(Path(tempfile.gettempdir())/'delivery-npc-server-test'), cleanup=Mock())
        self.service.events = queue.Queue()
        self.calls = []
        self.turn_count = 0
        self.on_start = lambda thread, turn: None
        self.on_interrupt = lambda thread, turn: None
        self.on_thread = lambda: None
        self.thread_tier_override = 'echo'
        self.config_result = {'config': {'mcp_servers': {'editor.tool': {'url': 'not-retained', 'env': {'KEY': 'not-retained'}}},
                                        'developer_instructions': 'not-retained'}}
        self.catalog_pages = {None: {'data': [{'id': model, 'model': model,
            'supportedReasoningEfforts': [{'reasoningEffort': 'low'}, {'reasoningEffort': 'high'}],
            'defaultReasoningEffort': 'high', 'serviceTiers': [{'id': 'priority', 'name': 'Fast'}],
            'defaultServiceTier': 'priority'} for model in ('gpt-6-luna', 'gpt-6.1-sol')], 'nextCursor': None}}
        patch.dict(os.environ, {'DELIVERY_CODEX_REASONING_EFFORT': 'auto', 'DELIVERY_CODEX_SERVICE_TIER': 'default'}).start()
        self.print = patch('app_server.print', create=True).start()
        self.kill = patch('app_server.subprocess.run').start()
        self.spawn = patch('app_server.subprocess.Popen', side_effect=AssertionError('unexpected_process')).start()
        self.addCleanup(patch.stopall)

    def respond(self, message):
        self.calls.append(message)
        method = message['method']
        if method == 'config/read':
            self.service.events.put({'id': message['id'], 'result': self.config_result})
        elif method == 'model/list':
            self.service.events.put({'id': message['id'], 'result': self.catalog_pages[message['params'].get('cursor')]})
        elif method == 'thread/start':
            self.turn_count += 1
            self.on_thread()
            tier = message['params']['serviceTier'] if self.thread_tier_override == 'echo' else self.thread_tier_override
            self.service.events.put({'id': message['id'], 'result': {'thread': {'id': f'thread-{self.turn_count}'},
                                                                    'serviceTier': tier}})
        elif method == 'turn/start':
            thread, turn = message['params']['threadId'], f'turn-{self.turn_count}'
            self.service.events.put({'id': message['id'], 'result': {'turn': {'id': turn}}})
            self.on_start(thread, turn)
        elif method == 'turn/interrupt':
            # interrupt 的 RPC 确认只表示收到请求，真正停止仍要等 turn/completed
            self.service.events.put({'id': message['id'], 'result': {}})
            self.on_interrupt(message['params']['threadId'], message['params']['turnId'])

    def completed_reply(self, thread, turn):
        self.service.events.put(notification('item/agentMessage/delta', thread, turn, delta='新回复'))
        self.service.events.put(terminal(thread, turn, 'completed'))

    def assert_process_closed(self):
        self.assertIsNone(self.service.process)
        self.process.wait.assert_called_once_with(timeout=3)
        if os.name == 'nt':
            self.assertEqual(self.kill.call_args.args[0], ['taskkill', '/PID', '123456', '/T', '/F'])
        else:
            self.process.kill.assert_called_once()

    def test_session_disables_inherited_mcp_without_copying_configuration_values(self):
        self.on_start = self.completed_reply
        for _ in range(2):
            self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None, model='gpt-6.1-sol')
        starts = [call['params'] for call in self.calls if call['method'] == 'thread/start']
        for params in starts:
            self.assertEqual(params['config'], {'mcp_servers': {'editor.tool': {'enabled': False}}})
            self.assertEqual(params['developerInstructions'], '')
            self.assertEqual(params['baseInstructions'], '角色规则')
        self.assertNotIn('not-retained', json.dumps(self.service.session_config))
        self.assertEqual(sum(call['method'] == 'config/read' for call in self.calls), 1)
        self.assertEqual(sum(call['method'] == 'model/list' for call in self.calls), 1)
        self.assertEqual(self.service.last_model_settings,
                         {'model': 'gpt-6.1-sol', 'effort': 'low', 'disabled_mcp_count': 1,
                          'requested_service_tier': 'default', 'thread_service_tier': 'default'})

    def test_default_tier_does_not_inherit_paid_global_or_catalog_default(self):
        self.config_result['config']['service_tier'] = 'priority'
        self.on_start = self.completed_reply
        with patch.dict(os.environ):
            os.environ.pop('DELIVERY_CODEX_SERVICE_TIER')
            self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None, model='gpt-6.1-sol')
        starts = {call['method']: call['params'] for call in self.calls if call['method'] in ('thread/start', 'turn/start')}
        self.assertEqual(starts['thread/start']['serviceTier'], 'default')
        self.assertEqual(starts['turn/start']['serviceTierForTurn'], 'default')
        self.assertEqual(starts['turn/start']['effort'], 'low')
        self.assertEqual(self.config_result['config']['service_tier'], 'priority')

    def test_fast_alias_sends_only_advertised_tier_and_records_thread_echo(self):
        self.on_start = self.completed_reply
        for option, advertised in (('fast', 'priority'), ('priority', 'fast'), ('fast', 'fast')):
            with self.subTest(option=option, advertised=advertised):
                for item in self.catalog_pages[None]['data']:
                    item['serviceTiers'] = [{'id': advertised, 'name': 'Fast'}]
                self.service.model_catalog = None
                with patch.dict(os.environ, {'DELIVERY_CODEX_SERVICE_TIER': option}):
                    self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None, model='gpt-6.1-sol')
                starts = {call['method']: call['params'] for call in self.calls if call['method'] in ('thread/start', 'turn/start')}
                self.assertEqual(starts['thread/start']['serviceTier'], advertised)
                self.assertEqual(starts['turn/start']['serviceTierForTurn'], advertised)
                self.assertEqual(self.service.last_model_settings['requested_service_tier'], advertised)
                self.assertEqual(self.service.last_model_settings['thread_service_tier'], advertised)
                logged = json.loads(self.print.call_args.args[0].removeprefix('model_settings '))
                self.assertEqual(logged, self.service.last_model_settings)

    def test_fast_requires_this_models_advertised_service_tier(self):
        self.catalog_pages[None]['data'][1]['serviceTiers'] = []
        self.catalog_pages[None]['data'][1]['additionalSpeedTiers'] = ['fast']
        with patch.dict(os.environ, {'DELIVERY_CODEX_SERVICE_TIER': 'fast'}):
            with self.assertRaisesRegex(ValueError, 'unsupported_model_service_tier'):
                self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None, model='gpt-6.1-sol')
        self.assertFalse(any(call['method'] in ('thread/start', 'turn/start') for call in self.calls))
        self.assert_process_closed()
        self.assertEqual(self.service.model_service_tiers, {})

    def test_unknown_tier_never_starts_thread_or_turn(self):
        with patch.dict(os.environ, {'DELIVERY_CODEX_SERVICE_TIER': 'ultrafast'}):
            with self.assertRaisesRegex(ValueError, 'unsupported_model_service_tier'):
                self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None)
        self.assertFalse(any(call['method'] in ('thread/start', 'turn/start') for call in self.calls))
        self.assert_process_closed()

    def test_missing_thread_tier_is_not_reported_as_confirmed(self):
        self.thread_tier_override = None
        self.on_start = self.completed_reply
        self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None)
        self.assertEqual(self.service.last_model_settings['requested_service_tier'], 'default')
        self.assertIsNone(self.service.last_model_settings['thread_service_tier'])

    def test_unexpected_paid_thread_tier_stops_before_model_turn(self):
        self.thread_tier_override = 'priority'
        with self.assertRaisesRegex(ValueError, 'unexpected_model_service_tier'):
            self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None)
        self.assertFalse(any(call['method'] == 'turn/start' for call in self.calls))
        self.assertEqual(self.service.last_model_settings['requested_service_tier'], 'default')
        self.assertEqual(self.service.last_model_settings['thread_service_tier'], 'priority')
        self.assert_process_closed()

    def test_effort_uses_supported_catalog_options_and_reads_all_pages(self):
        self.catalog_pages[None] = {'data': [], 'nextCursor': 'next-page'}
        self.catalog_pages['next-page'] = {'data': [{'id': 'catalog-id', 'model': 'test-model',
            'supportedReasoningEfforts': [{'reasoningEffort': 'high'}, {'reasoningEffort': 'minimal'}],
            'defaultReasoningEffort': 'high'}], 'nextCursor': None}
        self.on_start = self.completed_reply
        self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None, model='test-model')
        turn = next(call for call in self.calls if call['method'] == 'turn/start')
        self.assertEqual(turn['params']['effort'], 'minimal')
        self.assertEqual(sum(call['method'] == 'model/list' for call in self.calls), 2)

    def test_explicit_unsupported_effort_never_starts_inference(self):
        with patch.dict(os.environ, {'DELIVERY_CODEX_REASONING_EFFORT': 'none'}):
            with self.assertRaisesRegex(ValueError, 'unsupported_model_reasoning_effort'):
                self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None, model='gpt-6.1-sol')
        self.assertFalse(any(call['method'] in ('thread/start', 'turn/start') for call in self.calls))
        self.assert_process_closed()

    def test_explicit_supported_effort_is_preserved(self):
        self.on_start = self.completed_reply
        with patch.dict(os.environ, {'DELIVERY_CODEX_REASONING_EFFORT': 'high'}):
            self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None, model='gpt-6.1-sol')
        turn = next(call for call in self.calls if call['method'] == 'turn/start')
        self.assertEqual(turn['params']['effort'], 'high')

    def test_catalog_cursor_cycle_closes_process_without_inference(self):
        self.catalog_pages[None] = self.catalog_pages['again'] = {'data': [], 'nextCursor': 'again'}
        with self.assertRaisesRegex(ValueError, 'invalid_model_catalog_cursor'):
            self.service.stream('玩家发言', '角色规则', threading.Event(), lambda _: None)
        self.assertFalse(any(call['method'] == 'turn/start' for call in self.calls))
        self.assert_process_closed()
        self.assertIsNone(self.service.session_config)
        self.assertIsNone(self.service.model_catalog)

    def test_confirmed_interrupt_waits_for_own_terminal_and_reuses_process(self):
        cancel = threading.Event()
        interrupt_sent = threading.Event()
        finished = threading.Event()
        errors, replies = [], []
        self.on_start = lambda thread, turn: self.service.events.put(
            notification('item/agentMessage/delta', thread, turn, delta='首句'))
        self.on_interrupt = lambda thread, turn: interrupt_sent.set()

        def received(text):
            replies.append(text)
            cancel.set()

        def run():
            try:
                self.service.stream('玩家发言', '规则', cancel, received)
            except RuntimeError as error:
                errors.append(str(error))
            finally:
                finished.set()

        worker = threading.Thread(target=run, daemon=True)
        worker.start()
        try:
            self.assertTrue(interrupt_sent.wait(1))
            self.service.events.put(terminal('old-thread', 'turn-1', 'interrupted'))
            self.service.events.put(terminal('thread-1', 'old-turn', 'interrupted'))
            self.assertFalse(finished.wait(.1))
            self.service.events.put(terminal('thread-1', 'turn-1', 'interrupted'))
            self.assertTrue(finished.wait(1))
        finally:
            self.service.events.put(terminal('thread-1', 'turn-1', 'interrupted'))
            worker.join(1)
        self.assertEqual(errors, ['cancelled'])
        self.assertEqual(replies, ['首句'])
        self.assertIs(self.service.process, self.process)
        self.kill.assert_not_called()
        self.on_start = self.completed_reply
        self.service.stream('下一句话', '规则', threading.Event(), replies.append)
        self.assertEqual(replies, ['首句', '新回复'])
        self.assertEqual(sum(call['method'] == 'thread/unsubscribe' for call in self.calls), 2)
        self.spawn.assert_not_called()

    def test_interrupt_without_terminal_closes_process_on_deadline(self):
        cancel = threading.Event()
        clock = [0.0]
        interrupted = threading.Event()
        self.on_start = lambda thread, turn: self.service.events.put(
            notification('item/agentMessage/delta', thread, turn, delta='首句'))
        self.on_interrupt = lambda thread, turn: interrupted.set()
        original_read = self.service._read

        def read(deadline, token=None):
            if interrupted.is_set():
                clock[0] = 4.0
                raise TimeoutError('app_server_timeout')
            return original_read(deadline, token)

        # 直接推进测试时钟，检查三秒截止后的关闭路径
        with patch('app_server.time.monotonic', side_effect=lambda: clock[0]), patch.object(self.service, '_read', side_effect=read):
            with self.assertRaises(TimeoutError):
                self.service.stream('玩家发言', '规则', cancel, lambda _: cancel.set())
        self.assert_process_closed()

    def test_completed_turn_racing_interrupt_reuses_process_without_delivering_more_text(self):
        cancel = threading.Event()
        replies = []
        stages = []

        def start(thread, turn):
            self.service.events.put(notification('item/agentMessage/delta', thread, turn, delta='首句'))
            # 模型先完成，取消请求随后到达，合法的终态仍是 completed
            self.service.events.put(notification('item/agentMessage/delta', thread, turn, delta='取消后的旧句'))
            self.service.events.put(terminal(thread, turn, 'completed'))

        def received(text):
            replies.append(text)
            cancel.set()

        self.on_start = start
        with self.assertRaisesRegex(RuntimeError, '^cancelled$'):
            self.service.stream('玩家发言', '规则', cancel, received,
                                measure=lambda stage, _: stages.append(stage))
        self.assertEqual(replies, ['首句'])
        self.assertEqual(stages[-3:], ['cancel_requested', 'model_terminal', 'cancel_terminal_completed'])
        self.assertIs(self.service.process, self.process)
        self.kill.assert_not_called()
        self.assertEqual(sum(call['method'] == 'turn/interrupt' for call in self.calls), 1)
        self.on_start = self.completed_reply
        self.service.stream('下一句话', '规则', threading.Event(), replies.append)
        self.assertEqual(replies, ['首句', '新回复'])
        self.spawn.assert_not_called()

    def test_cancel_during_read_does_not_deliver_the_returned_delta(self):
        cancel = threading.Event()
        replies = []
        self.on_start = lambda thread, turn: self.service.events.put(
            notification('item/agentMessage/delta', thread, turn, delta='已取消的首句'))
        self.on_interrupt = lambda thread, turn: self.service.events.put(terminal(thread, turn, 'interrupted'))
        original_read = self.service._read

        def read(deadline, token=None):
            message = original_read(deadline, token)
            # 对应取消发生在阻塞读中、消息刚到达的时序
            if message.get('method') == 'item/agentMessage/delta':
                cancel.set()
            return message

        with patch.object(self.service, '_read', side_effect=read):
            with self.assertRaisesRegex(RuntimeError, '^cancelled$'):
                self.service.stream('玩家发言', '规则', cancel, replies.append)
        self.assertEqual(replies, [])
        self.assertIs(self.service.process, self.process)
        self.kill.assert_not_called()
        self.assertEqual(sum(call['method'] == 'turn/interrupt' for call in self.calls), 1)

    def test_old_thread_and_turn_content_is_not_delivered(self):
        def start(thread, turn):
            self.service.events.put(notification('item/agentMessage/delta', 'old-thread', turn, delta='旧会话'))
            self.service.events.put(notification('item/agentMessage/delta', thread, 'old-turn', delta='旧轮次'))
            self.service.events.put(notification('item/completed', thread, 'old-turn', item={'type': 'agentMessage', 'text': '旧整句'}))
            self.service.events.put(terminal(thread, 'old-turn', 'completed'))
            self.completed_reply(thread, turn)
        self.on_start = start
        replies = []
        self.service.stream('玩家发言', '规则', threading.Event(), replies.append)
        self.assertEqual(replies, ['新回复'])
        self.assertIs(self.service.process, self.process)

    def test_cancel_while_creating_thread_closes_process(self):
        cancel = threading.Event()
        self.on_thread = cancel.set
        with self.assertRaisesRegex(RuntimeError, 'cancelled'):
            self.service.stream('玩家发言', '规则', cancel, lambda _: None)
        self.assert_process_closed()

    def test_invalid_model_output_closes_process(self):
        self.on_start = self.completed_reply
        def reject(_):
            raise ValueError('invalid_candidate')
        with self.assertRaisesRegex(ValueError, 'invalid_candidate'):
            self.service.stream('玩家发言', '规则', threading.Event(), reject)
        self.assert_process_closed()

    def test_unsolicited_interrupted_terminal_closes_process(self):
        self.on_start = lambda thread, turn: self.service.events.put(terminal(thread, turn, 'interrupted'))
        with self.assertRaisesRegex(RuntimeError, 'model_interrupted'):
            self.service.stream('玩家发言', '规则', threading.Event(), lambda _: None)
        self.assert_process_closed()

    def test_measurements_are_ordered_and_callback_errors_do_not_change_reply(self):
        self.on_start = self.completed_reply
        timings, replies = [], []
        def measure(stage, elapsed_ms):
            timings.append((stage, elapsed_ms))
            raise ValueError('recorder_unavailable')
        self.service.stream('玩家发言', '规则', threading.Event(), replies.append, measure=measure)
        self.assertEqual([stage for stage, _ in timings], ['startup', 'thread_start', 'turn_start', 'first_delta', 'model_terminal'])
        self.assertEqual([elapsed for _, elapsed in timings], sorted(elapsed for _, elapsed in timings))
        self.assertEqual(replies, ['新回复'])
        self.assertIs(self.service.process, self.process)
        self.kill.assert_not_called()

    def test_token_usage_keeps_last_and_thread_total_separate(self):
        usage = token_usage()
        usage['last']['unknownText'] = 'must not be saved'
        def start(thread, turn):
            self.service.events.put(notification('thread/tokenUsage/updated', thread, turn, tokenUsage=usage))
            self.completed_reply(thread, turn)
        self.on_start = start
        replies = []
        self.service.stream('玩家发言', '规则', threading.Event(), replies.append)
        expected = token_usage()
        expected['last']['cacheWriteInputTokens'] = 0
        self.assertEqual(self.service.last_token_usage, expected)
        self.assertEqual(replies, ['新回复'])
        usage['total']['totalTokens'] = 999
        self.assertEqual(self.service.last_token_usage['total']['totalTokens'], 260)

    def test_token_usage_requires_matching_thread_and_explicit_turn(self):
        def start(thread, turn):
            for other_thread, other_turn in (('other-thread', turn), (thread, 'other-turn'), (thread, None)):
                self.service.events.put(notification('thread/tokenUsage/updated', other_thread, other_turn,
                                                      tokenUsage=token_usage()))
            self.completed_reply(thread, turn)
        self.on_start = start
        self.service.stream('玩家发言', '规则', threading.Event(), lambda _: None)
        self.assertIsNone(self.service.last_token_usage)

    def test_malformed_token_usage_does_not_replace_valid_counts_or_stop_reply(self):
        invalid = [None, [], {'last': {}}, {'last': token_usage()['last'], 'total': []}]
        for bad in (-1, True, 1.5, float('nan'), float('inf'), '120', 1 << 63):
            usage = token_usage()
            usage['total']['inputTokens'] = bad
            invalid.append(usage)
        missing = token_usage()
        del missing['last']['outputTokens']
        invalid.append(missing)
        def start(thread, turn):
            self.service.events.put(notification('thread/tokenUsage/updated', thread, turn, tokenUsage=token_usage()))
            self.service.events.put({'method': 'thread/tokenUsage/updated', 'params': []})
            for usage in invalid:
                self.service.events.put(notification('thread/tokenUsage/updated', thread, turn, tokenUsage=usage))
            self.completed_reply(thread, turn)
        self.on_start = start
        replies = []
        self.service.stream('玩家发言', '规则', threading.Event(), replies.append)
        self.assertEqual(self.service.last_token_usage['last']['totalTokens'], 145)
        self.assertEqual(self.service.last_token_usage['total']['inputTokens'], 220)
        self.assertEqual(replies, ['新回复'])
        self.assertIs(self.service.process, self.process)

    def test_next_stream_and_cancelled_entry_clear_previous_token_usage(self):
        def start(thread, turn):
            self.service.events.put(notification('thread/tokenUsage/updated', thread, turn, tokenUsage=token_usage()))
            self.completed_reply(thread, turn)
        self.on_start = start
        self.service.stream('上一句话', '规则', threading.Event(), lambda _: None)
        self.assertIsNotNone(self.service.last_token_usage)
        # 上一轮迟到的统计在新会话开始时被丢弃
        self.service.events.put(notification('thread/tokenUsage/updated', 'thread-1', 'turn-1', tokenUsage=token_usage()))
        self.on_start = self.completed_reply
        self.service.stream('下一句话', '规则', threading.Event(), lambda _: None)
        self.assertIsNone(self.service.last_token_usage)
        self.service.last_token_usage = token_usage()
        cancel = threading.Event()
        cancel.set()
        with self.assertRaisesRegex(RuntimeError, 'cancelled'):
            self.service.stream('已经取消', '规则', cancel, lambda _: None)
        self.assertIsNone(self.service.last_token_usage)


if __name__ == '__main__':
    unittest.main()
