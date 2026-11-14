"""原生模型按顺序生成声音，可选择只保留当前说话者的模型"""
import base64
import ctypes as C
import sys
import time
import wave

from neural_audio_sender import AudioChunkSender
from neural_tts import MAX_STREAM_CHUNKS, NeuralTtsError, project_path
from native_tts import native_identities, file_hash
from native_tts_abi import NativeLibrary, InitParams, TtsParams, Audio, VoiceReference, AudioCallback, CancelCallback


class NativeVoiceEngines:
    def __init__(self, config, runtime):
        import numpy as np
        self.np, self.config, self.runtime = np, config, runtime
        self.identities = native_identities(config, runtime)
        self.library = NativeLibrary(project_path(runtime['library']['path']))
        self.api = self.library.lib
        self.contexts = {}
        self.reference = VoiceReference()
        self.voice_residency = runtime.get('voice_residency', 'both')
        self._closed = False
        self._reference_owned = False
        try:
            if self.voice_residency not in ('both', 'active'):
                raise NeuralTtsError('native_tts_invalid_runtime')
            if self.voice_residency == 'both':
                self._load_voice('male')
            self._load_voice('female')
            entry = config['voices']['female']
            path = project_path(entry['reference_path'])
            if file_hash(path) != entry['reference_sha256']:
                raise NeuralTtsError('neural_tts_clone_reference_changed')
            with wave.open(str(path), 'rb') as audio:
                if audio.getparams()[:3] != (1, 2, 24000):
                    raise NeuralTtsError('neural_tts_clone_reference_format')
                samples = np.frombuffer(audio.readframes(audio.getnframes()), dtype='<i2').astype(np.float32) / 32768
            self._reference_owned = True
            if self.api.qt_extract_voice_ref(self.contexts['female'],
                    samples.ctypes.data_as(C.POINTER(C.c_float)), len(samples), C.byref(self.reference)):
                self._failure('native_tts_reference_failed')
            if self.voice_residency == 'active':
                # 声音参考由 C 接口独立分配，释放女声模型后仍可给下一次加载使用
                self._release_contexts()
                self._load_voice('male')
        except BaseException:
            self.close()
            raise

    def _load_voice(self, voice, check=None):
        model = self.runtime['models'][voice]
        revision = self.config['voices'][voice].get('model_revision', self.config['model_revision'])
        if model['source_revision'] != revision:
            raise NeuralTtsError('native_tts_model_revision_changed')
        params = InitParams()
        self.api.qt_init_default_params(C.byref(params))
        params.talker_path = str(project_path(model['path'])).encode('utf-8')
        params.codec_path = str(project_path(self.runtime['models']['codec']['path'])).encode('utf-8')
        params.device, params.max_batch = b'CUDA0', 1
        # qt_init 没有取消回调；父进程仍负责超时退出，返回后先检查这句是否已被取消
        handle = self.api.qt_init(C.byref(params))
        if not handle:
            self._failure('native_tts_model_load_failed')
        try:
            if check:
                check()
            expected = b'custom_voice' if voice == 'male' else b'base'
            if self.api.qt_model_type(handle) != expected:
                raise NeuralTtsError('native_tts_model_type_changed')
            if voice == 'male':
                speakers = {self.api.qt_speaker_name(handle, i).decode('utf-8').lower()
                            for i in range(self.api.qt_n_speakers(handle))}
                if self.config['voices']['male']['speaker'].lower() not in speakers:
                    raise NeuralTtsError('native_tts_speaker_missing')
        except BaseException:
            self.api.qt_free(handle)
            raise
        self.contexts[voice] = handle
        return handle

    def _release_contexts(self):
        handles = tuple(self.contexts.values())
        self.contexts.clear()
        for handle in handles:
            self.api.qt_free(handle)

    def activate_voice(self, voice, check):
        """同一人继续说话时复用模型，换人时先释放旧模型再加载"""
        check()
        if self._closed:
            raise NeuralTtsError('cancelled')
        if voice not in ('male', 'female'):
            raise NeuralTtsError('native_tts_unknown_voice')
        if voice not in self.contexts:
            if self.voice_residency == 'active':
                self._release_contexts()
            check()
            self._load_voice(voice, check)
        check()
        return self.contexts[voice]

    def _failure(self, code):
        print(f'native qwen error={self.api.qt_last_error()!r}', file=sys.stderr, flush=True)
        raise NeuralTtsError(code)

    def close(self):
        if self._closed:
            return
        self._closed = True
        self._release_contexts()
        if self._reference_owned:
            self._reference_owned = False
            self.api.qt_voice_ref_free(C.byref(self.reference))

    def synthesize(self, task, closed, on_chunk):
        np = self.np
        deadline = time.monotonic() + self.config['synthesis_timeout_seconds']
        started = time.monotonic()
        error = None
        sender = None
        count = total = 0
        first_audio_ms = None
        buffered = bytearray()

        def check():
            if self._closed or task.cancel.is_set() or closed.is_set():
                raise NeuralTtsError('cancelled')
            if error is not None:
                raise error
            if time.monotonic() >= deadline:
                raise NeuralTtsError('neural_tts_timeout')

        def cancelled(_):
            nonlocal error
            try:
                check()
                if sender:
                    sender.check_error()
                return False
            except Exception as failure:
                error = failure
                return True

        common = {'task_id': task.task_id, 'voice_name': task.speaker,
                  'voice_identity': self.identities[task.voice], 'sample_rate': 24000}

        def chunk(samples, sample_count, _):
            nonlocal error, count, total, first_audio_ms
            try:
                check()
                if sample_count <= 0 or sample_count > 8 * 1920:
                    raise NeuralTtsError('neural_tts_invalid_stream_size')
                values = np.ctypeslib.as_array(samples, shape=(sample_count,))
                if not np.isfinite(values).all():
                    raise NeuralTtsError('neural_tts_invalid_pcm')
                # 回调结束后原生缓冲区会复用，这里立即复制成独立的 PCM
                pcm = (np.clip(values, -1, 1) * 32767).astype('<i2').tobytes()
                if total + len(pcm) > self.config['max_pcm_bytes']:
                    raise NeuralTtsError('neural_tts_audio_too_long')
                if first_audio_ms is None:
                    first_audio_ms = (time.monotonic() - started) * 1000
                width = (task.chunk_frames or 8) * 3840
                for offset in range(0, len(pcm), width):
                    check()
                    piece = pcm[offset:offset + width]
                    if sender:
                        if count >= MAX_STREAM_CHUNKS:
                            raise NeuralTtsError('neural_tts_invalid_stream_size')
                        sender.enqueue(dict(common, type='audio_chunk', chunk_index=count, offset_bytes=total), piece)
                    else:
                        buffered.extend(piece)
                    count += 1
                    total += len(piece)
                return True
            except Exception as failure:
                error = failure
                return False

        voice_handle = self.activate_voice(task.voice, check)
        params, output = TtsParams(), Audio()
        self.api.qt_tts_default_params(C.byref(params))
        params.text, params.lang = task.text.encode('utf-8'), b'chinese'
        params.seed, params.max_new_tokens = self.config['seed'], self.config['max_new_tokens']
        params.temperature = params.subtalker_temperature = .9
        params.top_k = params.subtalker_top_k = 50
        params.top_p = params.subtalker_top_p = 1
        params.repetition_penalty = 1.05
        if task.voice == 'male':
            params.speaker = task.speaker.lower().encode('utf-8')
        else:
            params.ref_spk_emb, params.ref_spk_dim = self.reference.ref_spk_emb, self.reference.ref_spk_dim
            params.ref_codes, params.ref_T = self.reference.ref_codes, self.reference.ref_T
            params.ref_text = self.config['voices']['female']['reference_text'].encode('utf-8')
        # 两个 ctypes 回调保留到 qt_synthesize 返回，异常通过 error 带回主线程
        params.cancel, params.on_chunk = CancelCallback(cancelled), AudioCallback(chunk)
        check()
        if task.chunk_frames:
            sender = AudioChunkSender(on_chunk, check)
        try:
            check()
            status = self.api.qt_synthesize(voice_handle, C.byref(params), C.byref(output))
            check()
            if status:
                self._failure('neural_tts_generation_failed')
            if not total:
                raise NeuralTtsError('neural_tts_invalid_pcm')
            if sender:
                sender.finish()
                result = dict(common, type='audio_end', chunk_count=count, total_pcm_bytes=total)
            else:
                result = dict(common, type='audio', pcm=base64.b64encode(buffered).decode('ascii'))
            check()
            result['stream_metrics'] = {'first_audio_ms': round(first_audio_ms, 2),
                'synthesis_ms': round((time.monotonic() - started) * 1000, 2),
                'pcm_bytes': total, 'chunk_count': count}
            return result
        finally:
            if sender:
                sender.close()
            self.api.qt_audio_free(C.byref(output))
