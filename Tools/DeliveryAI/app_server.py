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
            mark('startup')
            result = self._rpc('thread/start', {'model': model or os.environ.get('DELIVERY_CODEX_MODEL') or 'gpt-6-luna',
                'cwd': self.folder.name, 'approvalPolicy': 'never', 'sandbox': 'read-only', 'ephemeral': True,
                'baseInstructions': instructions, 'serviceName': 'delivery_npc'}, cancel)
            thread = result['thread']['id']
            mark('thread_start')
            self.saved.clear()
            result = self._rpc('turn/start', {'threadId': thread, 'effort': 'low',
                'input': [{'type': 'text', 'text': prompt, 'text_elements': []}]}, cancel)
            turn = result['turn']['id']
            mark('turn_start')
            deadline = time.monotonic()+24
            interrupted = False
            received = False
            first_delta = False
            while True:
                if cancel.is_set() and not interrupted:
                    self._send('turn/interrupt', {'threadId': thread, 'turnId': turn})
                    interrupted = True
                    deadline = min(deadline, time.monotonic()+3)
                try:
                    message = self.saved.pop(0) if self.saved else self._read(min(deadline, time.monotonic()+.1))
                except TimeoutError:
                    if time.monotonic() >= deadline:
                        raise
                    continue
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
                    if interrupted and status == 'interrupted':
                        # 模型确认本轮已停止后保留进程，下次请求继续复用
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
