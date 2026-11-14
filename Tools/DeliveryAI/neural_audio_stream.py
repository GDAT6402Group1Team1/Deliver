"""从当前 Qwen 生成过程取出连续声音，模型与采样设置仍由原语音进程负责"""
import time

from neural_audio_sender import AudioChunkSender
from neural_tts import NeuralTtsError


class AudioStreamComplete(Exception):
    """官方生成已确认结束，流式调用直接完成尾音，省去再次整句解码"""

    def __init__(self, stream):
        self.stream = stream


class AudioChunkStream:
    """每次合成使用一个实例，退出时移除模型回调，不保留已发送的 PCM"""

    def __init__(self, engine, task, check, on_chunk):
        self.engine, self.task = engine, task
        self.check, self.on_chunk = check, on_chunk
        self.chunk_frames = getattr(task, 'chunk_frames', 0)
        if type(self.chunk_frames) is not int or self.chunk_frames not in (0, 4, 8):
            raise NeuralTtsError('neural_tts_invalid_chunk_frames')
        if self.chunk_frames and not callable(on_chunk):
            raise NeuralTtsError('neural_tts_stream_callback_missing')
        self.codes = []
        self.last_step = self.last_decoded_frame = 0
        self.chunk_count = self.total_bytes = 0
        self.reference_codes = None
        self.reference_frames = 0
        self.eos_observed = False
        self.finished = False
        self.hook = None
        self.sender = None
        self.tokenizer = None
        self.saved_decode = None
        self.had_decode_override = False
        self.final_codes_checked = False
        self.decode_calls = self.decoded_frames = self.copied_samples = 0
        self.decode_ms = 0.0

    def __enter__(self):
        if not self.chunk_frames:
            return self
        self.check()
        if hasattr(self.engine, 'prompt'):
            prompt = self.engine.prompt
            if len(prompt) != 1 or not prompt[0].icl_mode:
                raise NeuralTtsError('neural_tts_stream_layout_changed')
            self.reference_codes = prompt[0].ref_code
            if (self.reference_codes is None or self.reference_codes.ndim != 2
                    or self.reference_codes.shape[1] != 16):
                raise NeuralTtsError('neural_tts_stream_layout_changed')
            self.reference_frames = int(self.reference_codes.shape[0])
        self.tokenizer = self.engine.model.model.speech_tokenizer
        if (self.tokenizer.get_model_type() != 'qwen3_tts_tokenizer_12hz'
                or self.tokenizer.get_decode_upsample_rate() != 1920
                or self.tokenizer.model.get_output_sample_rate() != 24000):
            raise NeuralTtsError('neural_tts_stream_layout_changed')
        self.had_decode_override = 'decode' in vars(self.tokenizer)
        self.saved_decode = vars(self.tokenizer).get('decode')
        try:
            self.hook = self.engine.model.model.talker.register_forward_hook(self.observe, with_kwargs=True)
            self.tokenizer.decode = self.complete_generation
            self.sender = AudioChunkSender(self.on_chunk, self.check)
        except BaseException:
            self.__exit__()
            raise
        return self

    def __exit__(self, *_):
        if self.hook is not None:
            self.hook.remove()
            self.hook = None
        if self.tokenizer is not None:
            if self.had_decode_override:
                self.tokenizer.decode = self.saved_decode
            elif 'decode' in vars(self.tokenizer):
                delattr(self.tokenizer, 'decode')
            self.tokenizer = None
        self.finished = True
        self.codes.clear()
        if self.sender is not None:
            self.sender.close()

    def complete_generation(self, items):
        self.check()
        if (self.finished or self.final_codes_checked or not self.codes
                or not isinstance(items, list) or len(items) != 1
                or not isinstance(items[0], dict) or set(items[0]) != {'audio_codes'}):
            raise NeuralTtsError('neural_tts_stream_layout_changed')
        expected = self._all_codes()
        actual = items[0]['audio_codes']
        if tuple(actual.shape) != tuple(expected.shape) or not self.engine.torch.equal(actual, expected):
            raise NeuralTtsError('neural_tts_stream_layout_changed')
        self.final_codes_checked = True
        # 仅这次 synthesize 捕获该信号；外层上下文随后还原 tokenizer 的原方法
        raise AudioStreamComplete(self)

    def _all_codes(self):
        codes = self.engine.torch.stack(self.codes)
        if self.reference_codes is not None:
            codes = self.engine.torch.cat((self.reference_codes.to(codes.device), codes), dim=0)
        return codes

    def observe(self, _module, _args, _kwargs, output):
        self.check()
        if self.sender is not None:
            self.sender.check_error()
        if self.finished:
            raise NeuralTtsError('neural_tts_stream_layout_changed')
        hidden = output.hidden_states
        if not isinstance(hidden, tuple) or len(hidden) != 2:
            raise NeuralTtsError('neural_tts_stream_layout_changed')
        ids = hidden[1]
        if ids is None:
            if self.codes:
                raise NeuralTtsError('neural_tts_stream_layout_changed')
            return
        step = int(output.generation_step)
        if tuple(ids.shape) != (1, 16) or step != self.last_step + 1:
            raise NeuralTtsError('neural_tts_stream_layout_changed')
        self.last_step = step
        frame = ids[0].detach().clone()
        eos = self.engine.model.model.config.talker_config.codec_eos_token_id
        if int(frame[0].item()) == eos:
            self.eos_observed = True
            return
        if self.eos_observed or len(self.codes) >= self.engine.config['max_new_tokens']:
            raise NeuralTtsError('neural_tts_stream_layout_changed')
        self.codes.append(frame)
        if len(self.codes) - self.last_decoded_frame >= self.chunk_frames:
            self.decode_prefix()

    def decode_prefix(self):
        self.check()
        if self.finished or len(self.codes) <= self.last_decoded_frame:
            raise NeuralTtsError('neural_tts_stream_layout_changed')
        torch, np = self.engine.torch, self.engine.np
        generated_samples = len(self.codes) * 1920
        if generated_samples * 2 > self.engine.config['max_pcm_bytes']:
            raise NeuralTtsError('neural_tts_audio_too_long')
        codes = self._all_codes()
        started = time.monotonic()
        # 使用完整历史保持原有声音；在 GPU 上截取新增部分，只把这段声音复制到 CPU
        with torch.inference_mode():
            decoded = self.tokenizer.model.decode(codes.unsqueeze(0), return_dict=True)
        if len(decoded.audio_values) != 1:
            raise NeuralTtsError('neural_tts_invalid_pcm')
        values = decoded.audio_values[0]
        if values.ndim != 1 or int(values.shape[0]) != len(codes) * 1920:
            raise NeuralTtsError('neural_tts_stream_length_mismatch')
        start = self.reference_frames * 1920 + self.total_bytes // 2
        # cpu() 等待这段复制完成，后面不再额外等待整个 CUDA 设备
        values = values[start:].detach().float().cpu().numpy()
        self.decode_ms += (time.monotonic() - started) * 1000
        self.decode_calls += 1
        self.decoded_frames += len(codes)
        self.copied_samples += int(values.size)
        self.check()
        values = np.asarray(values, dtype=np.float32)
        if values.ndim != 1 or not values.size or not np.isfinite(values).all():
            raise NeuralTtsError('neural_tts_invalid_pcm')
        if generated_samples <= self.total_bytes // 2:
            raise NeuralTtsError('neural_tts_stream_length_mismatch')
        pcm = np.rint(np.clip(values, -1, 1) * 32767).astype('<i2').tobytes()
        self.send_pcm(pcm)
        self.last_decoded_frame = len(self.codes)

    def send_pcm(self, pcm):
        self.check()
        if (self.finished or not pcm or len(pcm) % 2 or self.chunk_count >= 32
                or len(pcm) > self.chunk_frames * 3840):
            raise NeuralTtsError('neural_tts_invalid_pcm')
        if self.total_bytes + len(pcm) > self.engine.config['max_pcm_bytes']:
            raise NeuralTtsError('neural_tts_audio_too_long')
        message = {'type': 'audio_chunk', 'task_id': self.task.task_id,
                   'chunk_index': self.chunk_count, 'offset_bytes': self.total_bytes,
                   'sample_rate': 24000, 'voice_name': self.task.speaker}
        self.sender.enqueue(message, pcm)
        self.check()
        self.total_bytes += len(pcm)
        self.chunk_count += 1

    def finish(self, full_pcm_bytes=None):
        self.check()
        expected_bytes = len(self.codes) * 3840
        if full_pcm_bytes is None:
            if not self.final_codes_checked:
                raise NeuralTtsError('neural_tts_stream_layout_changed')
            full_pcm_bytes = expected_bytes
        if (self.finished or not self.chunk_frames or not full_pcm_bytes
                or full_pcm_bytes % 2 or full_pcm_bytes != expected_bytes):
            raise NeuralTtsError('neural_tts_stream_length_mismatch')
        if full_pcm_bytes > self.engine.config['max_pcm_bytes']:
            raise NeuralTtsError('neural_tts_audio_too_long')
        # 原生成方法已确认 EOS，补发不足一块的尾音，随后核对整句长度
        if self.last_decoded_frame < len(self.codes):
            self.decode_prefix()
        self.check()
        if not self.chunk_count or self.total_bytes != full_pcm_bytes:
            raise NeuralTtsError('neural_tts_stream_length_mismatch')
        self.sender.finish()
        if self.sender.sent_count != self.chunk_count or self.sender.sent_bytes != self.total_bytes:
            raise NeuralTtsError('neural_tts_stream_length_mismatch')
        self.finished = True
        return {'type': 'audio_end', 'task_id': self.task.task_id,
                'chunk_count': self.chunk_count, 'total_pcm_bytes': self.total_bytes,
                'sample_rate': 24000, 'voice_name': self.task.speaker,
                'stream_metrics': {'decode_calls': self.decode_calls,
                    'decoded_frames': self.decoded_frames, 'copied_samples': self.copied_samples,
                    'decode_ms': round(self.decode_ms, 3),
                    'send_queue_wait_ms': round(self.sender.queue_wait_ms, 3),
                    'send_ms': round(self.sender.send_ms, 3)}}
