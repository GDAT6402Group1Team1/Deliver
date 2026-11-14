"""按配置加快 Talker 单步计算，保留 Qwen 原有的文本处理和采样"""
from contextlib import contextmanager
import hashlib
import importlib
import json
from pathlib import Path
import sys
import threading


COMMIT = 'a70afc0f81f7f5f8801c3227968f1102f43f211c'
FILES = {
    'talker_graph.py': '2e402ef29d80b076481ab55dc6d20f1c55cae56604b5c3540c24a13f674b3d73',
    'LICENSE': '442472a518bf71e371f2581aa0fcaf6ee2ef6854f78c340fdbe87c099950ea82',
}


def checked_vendor():
    directory = Path(__file__).with_name('vendor') / 'faster_qwen_v032'
    manifest = json.loads((directory / 'source-manifest.json').read_text(encoding='utf-8'))
    if manifest.get('commit') != COMMIT or manifest.get('license') != 'MIT':
        raise ValueError('Talker Graph 的源码版本或许可不符')
    recorded = {row['path']: row for row in manifest['files']}
    for name, expected in FILES.items():
        data = (directory / name).read_bytes()
        if (hashlib.sha256(data).hexdigest() != expected
                or recorded.get(name, {}).get('sha256') != expected
                or recorded[name]['bytes'] != len(data)):
            raise ValueError(f'Talker Graph 源码校验失败: {name}')
    return directory


def create_talker_graph(engine, max_seq_len=512):
    """只录制重复的单步计算，文本处理与声音编码采样仍由原 Qwen 生成流程负责"""
    directory = checked_vendor()
    sys.path.insert(0, str(directory.parent))
    try:
        module = importlib.import_module('faster_qwen_v032.talker_graph')
    finally:
        sys.path.remove(str(directory.parent))
    if Path(module.__file__).resolve() != (directory / 'talker_graph.py').resolve():
        raise ValueError('Talker Graph 导入位置不符')
    outer = engine.model.model.talker
    if (outer.config.num_hidden_layers != 28 or outer.config.hidden_size != 1024
            or outer.config.sliding_window is not None or not 512 <= max_seq_len <= 2048):
        raise ValueError('本次对照只覆盖当前 0.6B Talker 和单条输入')
    graph = module.TalkerGraph(outer.model, outer.config, device='cuda:0',
                              dtype=engine.torch.bfloat16, max_seq_len=max_seq_len)
    # 原 Qwen 使用整数位置；重放时直接复制原 forward 算好的位置
    graph.position_ids = engine.torch.zeros(3, 1, 1, dtype=engine.torch.long, device='cuda:0')
    with engine.torch.inference_mode():
        graph.capture(num_warmup=3)
    return graph


class TalkerForwardAdapter:
    """首步沿用原计算，后续单步复用 Graph；每句话结束后恢复原方法"""

    def __init__(self, engine, graph, output_type, compare_hidden=False):
        self.engine, self.graph, self.output_type = engine, graph, output_type
        self.outer = engine.model.model.talker
        self.model = self.outer.model
        self.original = self.model.forward
        self.thread_id = threading.get_ident()
        self.prefill_length = None
        self.next_position = None
        self.decode_steps = 0
        self.busy = False
        self.compare_hidden = compare_hidden
        self.reference_cache = None
        self.hidden_comparisons = []
        self.use_original = False

    def forward(self, *args, **kwargs):
        if threading.get_ident() != self.thread_id or self.busy:
            raise RuntimeError('Talker Graph 只允许创建它的线程依次生成一句话')
        self.busy = True
        try:
            return self._forward(args, kwargs)
        finally:
            self.busy = False

    def _forward(self, args, kwargs):
        if self.use_original:
            return self.original(*args, **kwargs)
        expected = {'input_ids', 'attention_mask', 'position_ids', 'past_key_values', 'inputs_embeds',
                    'use_cache', 'output_attentions', 'output_hidden_states', 'cache_position'}
        if (args or not expected.issubset(kwargs) or set(kwargs) - expected - {'return_dict'}
                or kwargs.get('return_dict', True) is not True or kwargs['input_ids'] is not None):
            raise ValueError('Talker forward 接口发生变化，停止对照')
        inputs = kwargs['inputs_embeds']
        if (inputs.ndim != 3 or inputs.shape[0] != 1 or inputs.shape[2] != self.graph.hidden_size
                or inputs.dtype != self.engine.torch.bfloat16
                or inputs.device != self.engine.torch.device('cuda:0')
                or kwargs['output_attentions'] or not kwargs['use_cache']):
            raise ValueError('Talker Graph 的输入类型、形状或缓存设置不符')
        if self.prefill_length is None:
            if inputs.shape[1] <= 1:
                raise ValueError('本次对照需要完整的第一步文本输入')
            if inputs.shape[1] + self.engine.config['max_new_tokens'] > self.graph.max_seq_len:
                # 长文本整句使用原计算和 DynamicCache，保留完整文本及原结束条件
                self.use_original = True
                self.prefill_length = int(inputs.shape[1])
                print(f'qwen talker_fallback reason=cache_limit prefill={self.prefill_length} '
                      f'limit={self.graph.max_seq_len}', file=sys.stderr, flush=True)
                return self.original(**kwargs)
            output = self.original(**kwargs)
            if self.compare_hidden:
                self.reference_cache = output.past_key_values
            self.prefill_length = self.graph.prefill_kv(output.past_key_values)
            if self.prefill_length != inputs.shape[1]:
                raise ValueError('第一步缓存长度与输入不符')
            self.next_position = self.prefill_length
            self.graph.set_generation_state(kwargs['attention_mask'], self.outer.rope_deltas)
            # HF 后续准备输入时也使用这份 StaticCache，避免同时维护两份不同的历史
            output.past_key_values = self.graph.static_cache
            return output
        if (inputs.shape[1] != 1 or kwargs['past_key_values'] is not self.graph.static_cache
                or self.next_position >= self.graph.max_seq_len
                or tuple(kwargs['cache_position'].shape) != (1,)
                or tuple(kwargs['position_ids'].shape) != (3, 1, 1)):
            raise ValueError('Talker 单步缓存或位置不符')
        if int(kwargs['cache_position'][0].item()) != self.next_position:
            raise ValueError('Talker 缓存位置没有按生成顺序前进')
        # 使用 HF 传入的缓存位置与 RoPE 位置；遮罩按同一位置选取，仍包含原左侧 padding
        self.graph.input_buf.copy_(inputs)
        self.graph.cache_position.copy_(kwargs['cache_position'])
        self.graph.position_ids.copy_(kwargs['position_ids'])
        self.graph._set_attention_mask(self.next_position)
        self.graph.graph.replay()
        hidden = self.graph.output_buf.clone()
        if self.compare_hidden:
            reference_kwargs = dict(kwargs, past_key_values=self.reference_cache)
            mask = reference_kwargs['attention_mask']
            if mask is not None and mask.ndim == 4:
                reference_kwargs['attention_mask'] = mask[..., :self.next_position + 1]
            reference = self.original(**reference_kwargs)
            self.reference_cache = reference.past_key_values
            difference = (hidden.float() - reference.last_hidden_state.float()).abs()
            graph_logits = self.outer.codec_head(hidden).float()
            reference_logits = self.outer.codec_head(reference.last_hidden_state).float()
            # 此模式逐步读回数字，会增加同步；只用于检查误差，不作为速度结果
            self.hidden_comparisons.append(dict(step=self.decode_steps,
                max_abs=float(difference.max().item()), mean_abs=float(difference.mean().item()),
                reference_max_abs=float(reference.last_hidden_state.abs().max().item()),
                logits_max_abs=float((graph_logits - reference_logits).abs().max().item()),
                same_argmax=bool(self.engine.torch.equal(graph_logits.argmax(-1), reference_logits.argmax(-1)))))
        self.next_position += 1
        self.decode_steps += 1
        # Qwen 最终只读取 hidden_states[-1]；clone 使后续重放不会覆盖此前生成记录
        return self.output_type(last_hidden_state=hidden, past_key_values=self.graph.static_cache,
                                hidden_states=(hidden,) if kwargs['output_hidden_states'] else None,
                                attentions=None)


@contextmanager
def use_talker_graph(engine, graph, *, output_type=None, compare_hidden=False):
    if output_type is None:
        from transformers.modeling_outputs import BaseModelOutputWithPast
        output_type = BaseModelOutputWithPast
    adapter = TalkerForwardAdapter(engine, graph, output_type, compare_hidden)
    had_override = 'forward' in vars(adapter.model)
    previous = vars(adapter.model).get('forward')
    adapter.model.forward = adapter.forward
    try:
        yield adapter
    finally:
        if had_override:
            adapter.model.forward = previous
        else:
            delattr(adapter.model, 'forward')
        # 同一流中的清零排在此前计算之后，下一句话重新填入自己的缓存
        with engine.torch.inference_mode():
            graph.static_cache.reset()
