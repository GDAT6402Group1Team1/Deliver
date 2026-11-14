"""复用本机 Codex App Server 进程，每次 NPC 请求使用独立临时会话"""
import json
import os
import queue
import shutil
import subprocess
import tempfile
import threading
import time


class AppServer:
    def __init__(self):
        self.process = None
        self.folder = None
        self.serial = 0
        self.saved = []
        self.last_token_usage = None
        self.session_config = None
        self.model_catalog = None
        self.model_service_tiers = {}
        self.last_model_settings = None

    def _prepare_session(self, model, cancel):
        """每个进程只发现一次配置与模型能力，不保留配置值或登录信息"""
        if self.session_config is None:
            result = self._rpc('config/read', {'includeLayers': False}, cancel)
            servers = result.get('config', {}).get('mcp_servers', {})
            if not isinstance(servers, dict):
                raise ValueError('invalid_mcp_configuration')
            # 空表不会清除继承的服务器，逐项关闭只作用于本次 NPC 会话
            self.session_config = {'mcp_servers': {name: {'enabled': False} for name in servers}}
        if self.model_catalog is None:
            catalog, service_tiers, cursor, visited = {}, {}, None, set()
            for _ in range(20):
                params = {'limit': 100, 'includeHidden': True}
                if cursor is not None:
                    params['cursor'] = cursor
                page = self._rpc('model/list', params, cancel)
                for item in page.get('data', []):
                    supported = tuple(option.get('reasoningEffort') for option in item.get('supportedReasoningEfforts', [])
                                      if isinstance(option.get('reasoningEffort'), str))
                    for name in (item.get('id'), item.get('model')):
                        if isinstance(name, str):
                            catalog[name] = (supported, item.get('defaultReasoningEffort'))
                            service_tiers[name] = tuple(option.get('id') for option in item.get('serviceTiers', [])
                                                        if isinstance(option, dict) and isinstance(option.get('id'), str))
                cursor = page.get('nextCursor')
                if cursor is None:
                    break
                if not isinstance(cursor, str) or cursor in visited:
                    raise ValueError('invalid_model_catalog_cursor')
                visited.add(cursor)
            else:
                raise ValueError('model_catalog_too_many_pages')
            self.model_catalog = catalog
            self.model_service_tiers = service_tiers
        supported, default = self.model_catalog.get(model, ((), None))
        # 默认保持原来的 low；需要对照时才显式请求 auto 或其他已公布档位
        requested = os.environ.get('DELIVERY_CODEX_REASONING_EFFORT', 'low')
        if requested == 'auto':
            effort = next((value for value in ('none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max', 'ultra')
                           if value in supported), default if default in supported else None)
        else:
            effort = requested if requested in supported else None
        if effort is None:
            raise ValueError('unsupported_model_reasoning_effort')
        # 标准档始终显式传入，不能继承用户全局的付费加速设置
        tier_option = os.environ.get('DELIVERY_CODEX_SERVICE_TIER', 'default')
        if tier_option == 'default':
            service_tier = 'default'
        elif tier_option in ('fast', 'priority'):
            advertised = self.model_service_tiers.get(model, ())
            service_tier = next((value for value in (tier_option, 'priority' if tier_option == 'fast' else 'fast')
                                 if value in advertised), None)
            if service_tier is None:
                raise ValueError('unsupported_model_service_tier')
        else:
            raise ValueError('unsupported_model_service_tier')
        self.last_model_settings = {'model': model, 'effort': effort,
                                    'disabled_mcp_count': len(self.session_config['mcp_servers']),
                                    'requested_service_tier': service_tier, 'thread_service_tier': None}
        return self.session_config, effort, service_tier

    def _record_token_usage(self, usage):
        # 只保存协议中的计数，异常统计不能影响本轮台词
        if not isinstance(usage, dict):
            return
        counts = {}
        fields = ('inputTokens', 'cachedInputTokens', 'outputTokens',
                  'reasoningOutputTokens', 'totalTokens', 'cacheWriteInputTokens')
        for group in ('last', 'total'):
            source = usage.get(group)
            if not isinstance(source, dict):
                return
            values = {}
            for field in fields:
                value = source.get(field, 0 if field == 'cacheWriteInputTokens' else None)
                if type(value) is not int or not 0 <= value <= (1 << 63)-1:
                    return
                values[field] = value
            counts[group] = values
        self.last_token_usage = counts

    def close(self):
        self.session_config = None
        self.model_catalog = None
        self.model_service_tiers = {}
        if self.process:
            if self.process.poll() is None:
                if os.name == 'nt':
                    subprocess.run(['taskkill', '/PID', str(self.process.pid), '/T', '/F'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   creationflags=subprocess.CREATE_NO_WINDOW, timeout=5)
                else:
                    self.process.kill()
            self.process.wait(timeout=3)
            self.process = None
        if self.folder:
            from pathlib import Path
            target = Path(self.folder.name).resolve()
            assert target.parent == Path(tempfile.gettempdir()).resolve() and target.name.startswith('delivery-npc-server-')
            self.folder.cleanup()
            self.folder = None

    def _send(self, method, params, request=True):
        self.serial += 1
        body = {'method': method, 'params': params}
        if request:
            body['id'] = self.serial
        self.process.stdin.write(json.dumps(body, ensure_ascii=False)+'\n')
        self.process.stdin.flush()
        return self.serial

    def _read(self, deadline, cancel=None):
        while time.monotonic() < deadline:
            if cancel is not None and cancel.is_set():
                raise RuntimeError('cancelled')
            if self.process.poll() is not None:
                raise RuntimeError('app_server_stopped')
            try:
                return self.events.get(timeout=.05)
            except queue.Empty:
                continue
        raise TimeoutError('app_server_timeout')

    def _rpc(self, method, params, cancel=None):
        if cancel is not None and cancel.is_set():
            raise RuntimeError('cancelled')
        ident = self._send(method, params)
        deadline = time.monotonic()+10
        while True:
            message = self._read(deadline, cancel)
            if message.get('id') == ident and 'method' not in message:
                if 'error' in message:
                    raise RuntimeError('app_server_'+method.replace('/', '_'))
                return message['result']
            self.saved.append(message)

    def _start(self, cancel=None):
        if cancel is not None and cancel.is_set():
            raise RuntimeError('cancelled')
        if self.process and self.process.poll() is None:
            return
        self.close()
        executable = os.environ.get('DELIVERY_CODEX_EXE') or shutil.which('codex')
        if not executable:
            raise RuntimeError('codex_not_installed')
        self.folder = tempfile.TemporaryDirectory(prefix='delivery-npc-server-')
        command = [executable, 'app-server', '--listen', 'stdio://',
                   '-c', 'approval_policy="never"', '-c', 'web_search="disabled"',
                   '-c', 'project_doc_max_bytes=0', '-c', 'features.skip_host_skill_discovery=true',
                   '-c', 'mcp_servers={}', '-c', 'developer_instructions=""']
        for feature in ('shell_tool', 'plugins', 'apps', 'multi_agent', 'hooks', 'image_generation', 'view_image', 'in_app_browser'):
            command += ['--disable', feature]
        self.events = queue.Queue(maxsize=4096)
        self.saved = []
        self.process = subprocess.Popen(command, cwd=self.folder.name, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding='utf-8',
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        process, events = self.process, self.events
        def read_lines():
            for line in process.stdout:
                if len(line) > 262144:
                    continue
                try:
                    events.put(json.loads(line), timeout=1)
                except (ValueError, queue.Full):
                    continue
        threading.Thread(target=read_lines, daemon=True).start()
        self._rpc('initialize', {'clientInfo': {'name': 'delivery_npc', 'title': 'Delivery NPC', 'version': '1.0'},
                                 'capabilities': {'experimentalApi': True}}, cancel)
        self._send('initialized', {}, request=False)

    def stream(self, prompt, instructions, cancel, on_delta, model=None, measure=None):
        """measure 接收阶段名与本次调用累计毫秒，只用于记录耗时"""
        # 即使这次在开始前取消，也不能沿用上一轮的用量
        self.last_token_usage = None
        self.last_model_settings = None
        if cancel.is_set():
            raise RuntimeError('cancelled')
        started = time.monotonic()
        def mark(stage):
            if measure is not None:
                try:
                    measure(stage, (time.monotonic()-started)*1000)
                except Exception:
                    # 计时记录失败不能影响台词和动作
                    pass
        thread = None
        cancel_confirmed = False
        try:
            # 建立连接和会话时也检查取消，玩家离开后及时结束等待
            self._start(cancel)
            selected_model = model or os.environ.get('DELIVERY_CODEX_MODEL') or 'gpt-6-luna'
            config, effort, service_tier = self._prepare_session(selected_model, cancel)
            mark('startup')
            result = self._rpc('thread/start', {'model': selected_model,
                'cwd': self.folder.name, 'approvalPolicy': 'never', 'sandbox': 'read-only', 'ephemeral': True,
                'baseInstructions': instructions, 'developerInstructions': '', 'config': config,
                'serviceName': 'delivery_npc', 'serviceTier': service_tier}, cancel)
            thread = result['thread']['id']
            # 这里只能确认会话回显，当前协议没有每轮实际计费档位通知
            reported_tier = result.get('serviceTier')
            if reported_tier in ('default', 'fast', 'priority'):
                self.last_model_settings['thread_service_tier'] = reported_tier
            print('model_settings '+json.dumps(self.last_model_settings, ensure_ascii=False), flush=True)
            if reported_tier is not None and (reported_tier not in ('default', 'fast', 'priority') or
                                               (service_tier == 'default' and reported_tier != 'default')):
                raise ValueError('unexpected_model_service_tier')
            mark('thread_start')
            self.saved.clear()
            result = self._rpc('turn/start', {'threadId': thread, 'effort': effort, 'serviceTierForTurn': service_tier,
                'input': [{'type': 'text', 'text': prompt, 'text_elements': []}]}, cancel)
            turn = result['turn']['id']
            mark('turn_start')
            deadline = time.monotonic()+24
            interrupted = False
            received = False
            first_delta = False

            def interrupt_if_cancelled():
                nonlocal interrupted, deadline
                if cancel.is_set() and not interrupted:
                    self._send('turn/interrupt', {'threadId': thread, 'turnId': turn})
                    interrupted = True
                    deadline = min(deadline, time.monotonic()+3)
                    mark('cancel_requested')

            while True:
                interrupt_if_cancelled()
                try:
                    message = self.saved.pop(0) if self.saved else self._read(min(deadline, time.monotonic()+.1))
                except TimeoutError:
                    if time.monotonic() >= deadline:
                        raise
                    continue
                # 取消可能发生在阻塞读期间，先停止本轮输出再处理刚收到的消息
                interrupt_if_cancelled()
                params = message.get('params', {})
                if not isinstance(params, dict):
                    continue
                if 'id' in message and 'method' in message:
                    # NPC 只能返回文字，任何额外工具请求都拒绝
                    self.process.stdin.write(json.dumps({'id': message['id'], 'error': {'code': -32601, 'message': 'Tools disabled'}})+'\n')
                    self.process.stdin.flush()
                if params.get('threadId') != thread:
                    continue
                # 旧轮次的迟到消息不能混进这次回复，未带 turnId 的旧格式仍按会话隔离
                if params.get('turnId', turn) != turn:
                    continue
                if message.get('method') == 'thread/tokenUsage/updated':
                    # 用量通知要求明确的轮次编号，不能按旧消息格式猜测归属
                    if params.get('turnId') == turn:
                        self._record_token_usage(params.get('tokenUsage'))
                    continue
                if message.get('method') == 'item/agentMessage/delta' and not interrupted:
                    received = True
                    delta = params.get('delta', '')
                    if delta and not first_delta:
                        first_delta = True
                        mark('first_delta')
                    on_delta(delta)
                if message.get('method') == 'item/completed' and not received and not interrupted:
                    item = params.get('item', {})
                    if item.get('type') == 'agentMessage':
                        text = item.get('text', '')
                        if text and not first_delta:
                            first_delta = True
                            mark('first_delta')
                        on_delta(text)
                if message.get('method') == 'turn/completed' and params.get('turn', {}).get('id') == turn:
                    status = params['turn']['status']
                    mark('model_terminal')
                    if interrupted and status in ('interrupted', 'completed'):
                        # 模型可能在取消到达前自然结束，同轮终态仍足以确认已停止
                        # 已取消的文本不再交付，进程可供下一轮独立会话继续使用
                        mark('cancel_terminal_'+status)
                        cancel_confirmed = True
                        raise RuntimeError('cancelled')
                    if interrupted or status != 'completed':
                        raise RuntimeError('model_'+status)
                    break
        except Exception:
            # 超时或连接失败时，关闭本次模型调用使用的进程
            if not cancel_confirmed:
                self.close()
            raise
        finally:
            if thread and self.process and self.process.poll() is None:
                try:
                    self._send('thread/unsubscribe', {'threadId': thread})
                except Exception:
                    self.close()
                    raise
