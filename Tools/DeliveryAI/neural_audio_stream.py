"""从当前 Qwen 生成过程取出连续声音，模型与采样设置仍由原语音进程负责"""
import base64

from neural_tts import NeuralTtsError


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
        self.hook = self.engine.model.model.talker.register_forward_hook(self.observe, with_kwargs=True)
        return self

    def __exit__(self, *_):
        if self.hook is not None:
            self.hook.remove()
            self.hook = None
        self.finished = True
        self.codes.clear()

    def observe(self, _module, _args, _kwargs, output):
        self.check()
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
        codes = torch.stack(self.codes)
        if self.reference_codes is not None:
            codes = torch.cat((self.reference_codes.to(codes.device), codes), dim=0)
        # 保留左侧声音作为解码上下文，只发送本次新增加的部分
        with torch.inference_mode():
            waves, rate = self.engine.model.model.speech_tokenizer.decode([{'audio_codes': codes}])
        self.engine.synchronize()
        self.check()
        if rate != 24000 or len(waves) != 1:
            raise NeuralTtsError('neural_tts_invalid_audio_format')
        values = waves[0]
        if hasattr(values, 'cpu'):
            values = values.detach().float().cpu().numpy()
        values = np.asarray(values, dtype=np.float32)
        if values.ndim != 1 or not values.size or not np.isfinite(values).all():
            raise NeuralTtsError('neural_tts_invalid_pcm')
        # Base 与官方完整生成采用相同方式去掉参考录音，参考声音不发给玩家
        reference_samples = int(self.reference_frames / len(codes) * values.size)
        generated_samples = values.size - reference_samples
        if generated_samples * 2 > self.engine.config['max_pcm_bytes']:
            raise NeuralTtsError('neural_tts_audio_too_long')
        start = reference_samples + self.total_bytes // 2
        if generated_samples <= self.total_bytes // 2:
            raise NeuralTtsError('neural_tts_stream_length_mismatch')
        pcm = np.rint(np.clip(values[start:], -1, 1) * 32767).astype('<i2').tobytes()
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
                   'pcm': base64.b64encode(pcm).decode('ascii'),
                   'sample_rate': 24000, 'voice_name': self.task.speaker}
        # 回调在当前生成线程执行，发送受阻时由父进程取消并监督本进程退出
        self.check()
        try:
            self.on_chunk(message)
        except NeuralTtsError:
            raise
        except Exception as error:
            raise NeuralTtsError('neural_tts_stream_callback_failed') from error
        self.check()
        self.total_bytes += len(pcm)
        self.chunk_count += 1

    def finish(self, full_pcm_bytes):
        self.check()
        if (self.finished or not self.chunk_frames or not full_pcm_bytes
                or full_pcm_bytes % 2):
            raise NeuralTtsError('neural_tts_stream_length_mismatch')
        if full_pcm_bytes > self.engine.config['max_pcm_bytes']:
            raise NeuralTtsError('neural_tts_audio_too_long')
        # 原生成方法已确认 EOS，补发不足一块的尾音，随后核对整句长度
        if self.last_decoded_frame < len(self.codes):
            self.decode_prefix()
        self.check()
        if not self.chunk_count or self.total_bytes != full_pcm_bytes:
            raise NeuralTtsError('neural_tts_stream_length_mismatch')
        self.finished = True
        return {'type': 'audio_end', 'task_id': self.task.task_id,
                'chunk_count': self.chunk_count, 'total_pcm_bytes': self.total_bytes,
                'sample_rate': 24000, 'voice_name': self.task.speaker}
