"""用 CPU 替身检查单步缓存、输出保存和退出恢复，不加载 CUDA"""
from contextlib import nullcontext
from types import SimpleNamespace
import unittest

import numpy as np

from neural_talker_graph import checked_vendor, use_talker_graph


class Tensor:
    def __init__(self, value):
        self.value = np.asarray(value)
        self.shape, self.ndim = self.value.shape, self.value.ndim
        self.dtype, self.device = 'bfloat16', 'cuda:0'

    def __getitem__(self, key):
        return Tensor(self.value[key])

    def item(self):
        return self.value.item()

    def clone(self):
        return Tensor(self.value.copy())

    def copy_(self, other):
        self.value[...] = other.value


def fixture():
    calls, resets, masks = [], [], []
    cache = SimpleNamespace(reset=lambda: resets.append(True))
    def original(**kwargs):
        calls.append(kwargs)
        return SimpleNamespace(last_hidden_state=kwargs['inputs_embeds'],
                               past_key_values=SimpleNamespace(length=kwargs['inputs_embeds'].shape[1]))
    model = SimpleNamespace(forward=original)
    engine = SimpleNamespace(model=SimpleNamespace(model=SimpleNamespace(
        talker=SimpleNamespace(model=model, rope_deltas=Tensor([[0]])))),
        torch=SimpleNamespace(bfloat16='bfloat16', device=lambda value: value, inference_mode=nullcontext),
        config={'max_new_tokens': 256})
    graph = SimpleNamespace(hidden_size=4, max_seq_len=512, static_cache=cache,
        input_buf=Tensor(np.zeros((1, 1, 4))), cache_position=Tensor([0]),
        position_ids=Tensor(np.zeros((3, 1, 1))), output_buf=Tensor(np.zeros((1, 1, 4))),
        prefill_kv=lambda source: source.length,
        set_generation_state=lambda mask, rope: None, _set_attention_mask=lambda position: masks.append(position))
    def replay():
        graph.output_buf.value[...] = graph.cache_position.value[0]
    graph.graph = SimpleNamespace(replay=replay)
    def inputs(length, position, past=None):
        return dict(input_ids=None, attention_mask=Tensor(np.ones((1, length))),
                    position_ids=Tensor(np.full((3, 1, length), position)), past_key_values=past,
                    inputs_embeds=Tensor(np.ones((1, length, 4))), use_cache=True,
                    output_attentions=False, output_hidden_states=True,
                    cache_position=Tensor(np.arange(position, position + length)), return_dict=True)
    return engine, graph, model, original, inputs, calls, resets, masks


class TalkerForwardGraphTests(unittest.TestCase):
    def test_fixed_source_and_license_match_manifest(self):
        self.assertTrue((checked_vendor() / 'talker_graph.py').is_file())

    def test_one_cache_and_each_saved_hidden_survives_next_replay(self):
        engine, graph, model, original, inputs, calls, resets, masks = fixture()
        with use_talker_graph(engine, graph, output_type=SimpleNamespace) as adapter:
            prefill = model.forward(**inputs(4, 0))
            self.assertIs(prefill.past_key_values, graph.static_cache)
            first = model.forward(**inputs(1, 4, prefill.past_key_values))
            second = model.forward(**inputs(1, 5, first.past_key_values))
            self.assertTrue(np.all(first.hidden_states[-1].value == 4))
            self.assertTrue(np.all(second.hidden_states[-1].value == 5))
            self.assertEqual(adapter.decode_steps, 2)
        self.assertEqual((len(calls), len(resets), masks), (1, 1, [4, 5]))
        self.assertIs(model.forward, original)

    def test_wrong_position_fails_and_restores_original_forward(self):
        engine, graph, model, original, inputs, _, resets, _ = fixture()
        with self.assertRaisesRegex(ValueError, '生成顺序'):
            with use_talker_graph(engine, graph, output_type=SimpleNamespace):
                model.forward(**inputs(4, 0))
                model.forward(**inputs(1, 6, graph.static_cache))
        self.assertIs(model.forward, original)
        self.assertEqual(len(resets), 1)

    def test_long_text_keeps_all_tokens_and_uses_original_cache_for_whole_utterance(self):
        engine, graph, model, original, inputs, calls, _, masks = fixture()
        with use_talker_graph(engine, graph, output_type=SimpleNamespace) as adapter:
            first = model.forward(**inputs(300, 0))
            model.forward(**inputs(1, 300, first.past_key_values))
            self.assertTrue(adapter.use_original)
            self.assertEqual(adapter.decode_steps, 0)
        self.assertEqual(calls[0]['inputs_embeds'].shape[1], 300)
        self.assertIsNot(first.past_key_values, graph.static_cache)
        self.assertIs(calls[1]['past_key_values'], first.past_key_values)
        self.assertEqual(masks, [])
        self.assertIs(model.forward, original)

    def test_cancel_restores_forward_and_next_utterance_uses_its_own_cache(self):
        engine, graph, model, original, inputs, calls, resets, _ = fixture()
        with self.assertRaisesRegex(RuntimeError, 'cancelled'):
            with use_talker_graph(engine, graph, output_type=SimpleNamespace):
                model.forward(**inputs(4, 0))
                raise RuntimeError('cancelled')
        self.assertIs(model.forward, original)
        with use_talker_graph(engine, graph, output_type=SimpleNamespace) as adapter:
            model.forward(**inputs(7, 0))
            self.assertEqual(adapter.next_position, 7)
        self.assertEqual((len(calls), len(resets)), (2, 2))


if __name__ == '__main__':
    unittest.main()
