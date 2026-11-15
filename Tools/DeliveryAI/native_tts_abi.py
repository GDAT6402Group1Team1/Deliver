"""固定版本 qwentts.cpp 的 C 接口，加载库不会创建模型"""
import ctypes as C
import os

COMMIT = 'f1a6924789883bd92eeb59176793a94af55730b3'
FloatPtr = C.POINTER(C.c_float)
IntPtr = C.POINTER(C.c_int32)
CancelCallback = C.CFUNCTYPE(C.c_bool, C.c_void_p)
AudioCallback = C.CFUNCTYPE(C.c_bool, FloatPtr, C.c_int, C.c_void_p)


class InitParams(C.Structure):
    _fields_ = [('abi_version', C.c_int), ('talker_path', C.c_char_p),
                ('codec_path', C.c_char_p), ('use_fa', C.c_bool), ('clamp_fp16', C.c_bool),
                ('max_batch', C.c_int), ('codec_chunk_sec', C.c_float), ('device', C.c_char_p)]


class Audio(C.Structure):
    _fields_ = [('samples', FloatPtr), ('n_samples', C.c_int),
                ('sample_rate', C.c_int), ('channels', C.c_int)]


class VoiceReference(C.Structure):
    _fields_ = [('ref_spk_emb', FloatPtr), ('ref_spk_dim', C.c_int),
                ('ref_codes', IntPtr), ('ref_T', C.c_int), ('num_codebooks', C.c_int)]


class TtsParams(C.Structure):
    _fields_ = [('abi_version', C.c_int), ('text', C.c_char_p), ('lang', C.c_char_p),
                ('instruct', C.c_char_p), ('speaker', C.c_char_p),
                ('ref_audio_24k', FloatPtr), ('ref_n_samples', C.c_int), ('ref_text', C.c_char_p),
                ('seed', C.c_int64), ('max_new_tokens', C.c_int), ('temperature', C.c_float),
                ('top_k', C.c_int), ('top_p', C.c_float), ('repetition_penalty', C.c_float),
                ('subtalker_temperature', C.c_float), ('subtalker_top_k', C.c_int),
                ('subtalker_top_p', C.c_float), ('dump_dir', C.c_char_p),
                ('cancel', CancelCallback), ('cancel_user_data', C.c_void_p),
                ('on_chunk', AudioCallback), ('on_chunk_user_data', C.c_void_p),
                ('ref_spk_emb', FloatPtr), ('ref_spk_dim', C.c_int),
                ('ref_codes', IntPtr), ('ref_T', C.c_int)]


class NativeLibrary:
    def __init__(self, path, *, shared_codec=False):
        self.directory = os.add_dll_directory(str(path.parent)) if os.name == 'nt' else None
        try:
            self.lib = C.CDLL(str(path))
            signatures = {
                'qt_version': ([], C.c_char_p), 'qt_last_error': ([], C.c_char_p),
                'qt_init_default_params': ([C.POINTER(InitParams)], None),
                'qt_init': ([C.POINTER(InitParams)], C.c_void_p),
                'qt_free': ([C.c_void_p], None),
                'qt_tts_default_params': ([C.POINTER(TtsParams)], None),
                'qt_synthesize': ([C.c_void_p, C.POINTER(TtsParams), C.POINTER(Audio)], C.c_int),
                'qt_audio_free': ([C.POINTER(Audio)], None),
                'qt_extract_voice_ref': ([C.c_void_p, FloatPtr, C.c_int, C.POINTER(VoiceReference)], C.c_int),
                'qt_voice_ref_free': ([C.POINTER(VoiceReference)], None),
                'qt_model_type': ([C.c_void_p], C.c_char_p),
                'qt_n_speakers': ([C.c_void_p], C.c_int),
                'qt_speaker_name': ([C.c_void_p, C.c_int], C.c_char_p),
            }
            for name, (args, result) in signatures.items():
                method = getattr(self.lib, name)
                method.argtypes, method.restype = args, result
            self.version = self.lib.qt_version().decode('utf-8')
            if not self.version.startswith(COMMIT[:7]):
                raise ValueError('native_tts_library_version_changed')
            initial, speech = InitParams(), TtsParams()
            self.lib.qt_init_default_params(C.byref(initial))
            self.lib.qt_tts_default_params(C.byref(speech))
            if initial.abi_version != 6 or speech.abi_version != 6:
                raise ValueError('native_tts_abi_changed')
            if shared_codec:
                try:
                    initialize = self.lib.qt_init_shared_codec
                except AttributeError as error:
                    raise ValueError('native_tts_shared_codec_unavailable') from error
                initialize.argtypes = [C.POINTER(InitParams), C.c_void_p]
                initialize.restype = C.c_void_p
        except BaseException:
            if self.directory:
                self.directory.close()
            raise
