"""实际加载 CPU 识别 DLL，检查流式结果、轮次隔离和错误边界"""

from __future__ import annotations

import array
import ctypes
import json
import math
import pathlib
import sys
import tempfile
import time
import unittest
import wave


ROOT = pathlib.Path(__file__).resolve().parents[2] / "Saved" / "DeliverySpeech"
RUNTIME = ROOT / "runtime"

OK, INVALID_ARGUMENT, INVALID_STATE, STALE_TURN, BUFFER_TOO_SMALL, MODEL_ERROR, RUNTIME_ERROR, LIMIT_EXCEEDED = range(8)
IDLE, STREAMING, FINAL, CANCELLED = range(4)


class Config(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("abi_version", ctypes.c_uint32),
                ("model_directory_utf8", ctypes.c_char_p), ("hotwords_utf8", ctypes.c_char_p),
                ("num_threads", ctypes.c_uint32), ("max_utterance_ms", ctypes.c_uint32),
                ("hotword_score", ctypes.c_float), ("reserved", ctypes.c_uint32)]


class ResultInfo(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("state", ctypes.c_uint32),
                ("turn_id", ctypes.c_uint64), ("revision", ctypes.c_uint64),
                ("input_sample_count", ctypes.c_uint64), ("input_sample_rate", ctypes.c_int32),
                ("reserved", ctypes.c_uint32)]


def load_api(path: pathlib.Path = RUNTIME / "DeliverySpeechRuntime.dll"):
    dll = ctypes.CDLL(str(path.resolve()))
    ptr = ctypes.c_void_p
    u64, u32, i32 = ctypes.c_uint64, ctypes.c_uint32, ctypes.c_int32
    declarations = {
        "dsr_abi_version": (u32, []),
        "dsr_create": (i32, [ctypes.POINTER(Config), ctypes.POINTER(ptr), ctypes.c_char_p, u32]),
        "dsr_begin": (i32, [ptr, u64]),
        "dsr_push": (i32, [ptr, u64, i32, ctypes.POINTER(ctypes.c_float), u32]),
        "dsr_finish": (i32, [ptr, u64]),
        "dsr_cancel": (i32, [ptr, u64]),
        "dsr_get_result": (i32, [ptr, ctypes.POINTER(ResultInfo), ctypes.c_char_p, u32, ctypes.POINTER(u32)]),
        "dsr_get_error": (i32, [ptr, ctypes.c_char_p, u32, ctypes.POINTER(u32)]),
        "dsr_destroy": (None, [ptr]),
    }
    for name, (result_type, argument_types) in declarations.items():
        function = getattr(dll, name)
        function.restype = result_type
        function.argtypes = argument_types
    return dll


def make_config(models: pathlib.Path = RUNTIME / "models", *, max_ms: int = 15000) -> Config:
    return Config(ctypes.sizeof(Config), 1, str(models.resolve()).encode("utf-8"),
                  "二狗\n小绿\n斧头\n快递".encode("utf-8"), 1, max_ms, 2.0, 0)


def create(dll, config: Config):
    handle = ctypes.c_void_p()
    error = ctypes.create_string_buffer(2048)
    status = dll.dsr_create(ctypes.byref(config), ctypes.byref(handle), error, len(error))
    return status, handle, error.value.decode("utf-8", errors="replace")


def get_result(dll, handle):
    info = ResultInfo(struct_size=ctypes.sizeof(ResultInfo))
    required = ctypes.c_uint32()
    status = dll.dsr_get_result(handle, ctypes.byref(info), None, 0, ctypes.byref(required))
    if status != BUFFER_TOO_SMALL or not required.value:
        raise AssertionError(f"Result sizing failed: {status}, {required.value}")
    output = ctypes.create_string_buffer(required.value)
    status = dll.dsr_get_result(handle, ctypes.byref(info), output, len(output), ctypes.byref(required))
    if status != OK:
        raise AssertionError(f"Result copy failed: {status}")
    return info, output.value.decode("utf-8")


def push(dll, handle, turn_id: int, samples, sample_rate: int = 16000):
    buffer = (ctypes.c_float * len(samples))(*samples)
    return dll.dsr_push(handle, turn_id, sample_rate, buffer, len(buffer))


def read_fixture():
    path = ROOT / "fixtures" / "3.wav"
    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2 or source.getframerate() != 16000:
            raise AssertionError("Pinned fixture must be 16 kHz mono PCM16")
        values = array.array("h", source.readframes(source.getnframes()))
        if sys.byteorder != "little":
            values.byteswap()
    return [sample / 32768.0 for sample in values]


class NativeSpeechTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform != "win32" or not (RUNTIME / "DeliverySpeechRuntime.dll").exists():
            raise unittest.SkipTest("Build the Win64 runtime before native integration tests")
        if not (ROOT / "fixtures" / "3.wav").exists():
            raise RuntimeError("Run prepare_runtime.py --fixtures first")
        cls.dll = load_api()
        started = time.monotonic()
        status, cls.handle, error = create(cls.dll, make_config())
        if status != OK:
            raise RuntimeError(f"Native initialization failed ({status}): {error}")
        cls.report = {"scope": "CPU DLL fixture/lifecycle integration; not live microphone, UE or AEC acceptance",
                      "model_init_ms": round((time.monotonic() - started) * 1000, 3), "decodes": []}
        cls.next_turn = 100
        cls.fixture = read_fixture()

    @classmethod
    def tearDownClass(cls):
        cls.dll.dsr_destroy(cls.handle)
        cls.dll.dsr_destroy(None)
        report_path = ROOT / "native_test_metrics.json"
        report_path.write_text(json.dumps(cls.report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(cls.report, ensure_ascii=False), flush=True)

    def setUp(self):
        type(self).next_turn += 1
        self.turn = type(self).next_turn
        self.assertEqual(self.dll.dsr_begin(self.handle, self.turn), OK)

    def tearDown(self):
        self.dll.dsr_cancel(self.handle, self.turn)

    def decode(self, turn, *, frame=320, rate=16000, leading_silence_seconds=0):
        samples = self.fixture if rate == 16000 else [value for value in self.fixture for _ in range(3)]
        started = time.monotonic()
        partials = []
        for _ in range(leading_silence_seconds):
            self.assertEqual(push(self.dll, self.handle, turn, [0.0] * rate, rate), OK)
            self.assertEqual(get_result(self.dll, self.handle)[1], "")
        for start in range(0, len(samples), frame):
            self.assertEqual(push(self.dll, self.handle, turn, samples[start:start + frame], rate), OK)
            info, text = get_result(self.dll, self.handle)
            self.assertEqual(info.state, STREAMING)
            if text and (not partials or text != partials[-1]):
                partials.append(text)
        release = time.monotonic()
        self.assertEqual(self.dll.dsr_finish(self.handle, turn), OK)
        ended = time.monotonic()
        info, text = get_result(self.dll, self.handle)
        self.assertEqual(info.state, FINAL)
        self.assertEqual(info.turn_id, turn)
        self.assertEqual(info.input_sample_count, len(samples) + leading_silence_seconds * rate)
        self.assertTrue(text)
        self.assertTrue(partials, "A streamed utterance must expose a partial before finish")
        self.report["decodes"].append({"input_rate": rate, "chunk_samples": frame, "text": text,
                                       "partial_updates": len(partials),
                                       "audio_seconds": len(samples) / rate + leading_silence_seconds,
                                       "compute_ms": round((ended - started) * 1000, 3),
                                       "finish_ms": round((ended - release) * 1000, 3)})
        return info, text

    def test_abi_and_empty_finish_are_idempotent(self):
        self.assertEqual(self.dll.dsr_abi_version(), 1)
        self.assertEqual(self.dll.dsr_begin(self.handle, self.turn), OK)
        self.assertEqual(self.dll.dsr_finish(self.handle, self.turn), OK)
        first, text = get_result(self.dll, self.handle)
        self.assertEqual(text, "")
        self.assertEqual(first.input_sample_count, 0)
        self.assertEqual(first.state, FINAL)
        self.assertEqual(self.dll.dsr_finish(self.handle, self.turn), OK)
        second, _ = get_result(self.dll, self.handle)
        self.assertEqual(first.revision, second.revision)
        self.assertEqual(self.dll.dsr_begin(self.handle, self.turn), INVALID_STATE)

    def test_cancel_and_late_callbacks_cannot_replace_new_turn(self):
        for start in range(0, 48000, 3200):
            self.assertEqual(push(self.dll, self.handle, self.turn, self.fixture[start:start + 3200]), OK)
        self.assertEqual(self.dll.dsr_cancel(self.handle, self.turn), OK)
        cancelled, text = get_result(self.dll, self.handle)
        self.assertEqual(cancelled.state, CANCELLED)
        self.assertEqual(text, "")
        self.assertEqual(self.dll.dsr_cancel(self.handle, self.turn), OK)
        self.assertEqual(self.dll.dsr_finish(self.handle, self.turn), INVALID_STATE)
        old_turn = self.turn
        type(self).next_turn += 1
        self.turn = type(self).next_turn
        self.assertEqual(self.dll.dsr_begin(self.handle, self.turn), OK)
        self.assertEqual(self.dll.dsr_cancel(self.handle, old_turn), STALE_TURN)
        self.assertEqual(self.dll.dsr_finish(self.handle, old_turn), STALE_TURN)
        self.assertEqual(push(self.dll, self.handle, old_turn, [0.0] * 160), STALE_TURN)
        current, text = get_result(self.dll, self.handle)
        self.assertEqual(current.turn_id, self.turn)
        self.assertEqual(current.state, STREAMING)
        self.assertEqual(current.input_sample_count, 0)

    def test_streaming_is_invariant_to_capture_chunk_boundaries(self):
        first, text = self.decode(self.turn, frame=320)
        self.assertEqual(self.dll.dsr_finish(self.handle, self.turn), OK)
        same, same_text = get_result(self.dll, self.handle)
        self.assertEqual((first.revision, text), (same.revision, same_text))
        type(self).next_turn += 1
        self.turn = type(self).next_turn
        self.assertEqual(self.dll.dsr_begin(self.handle, self.turn), OK)
        _, other_text = self.decode(self.turn, frame=5173)
        self.assertEqual(text, other_text)

    def test_begin_from_two_previous_turns_cannot_restart_recording(self):
        previous_turns = []
        for _ in range(2):
            previous_turns.append(self.turn)
            self.assertEqual(self.dll.dsr_cancel(self.handle, self.turn), OK)
            type(self).next_turn += 1
            self.turn = type(self).next_turn
            self.assertEqual(self.dll.dsr_begin(self.handle, self.turn), OK)
        self.assertEqual(push(self.dll, self.handle, self.turn, [0.0] * 160), OK)
        before, _ = get_result(self.dll, self.handle)
        for old_turn in previous_turns:
            self.assertEqual(self.dll.dsr_begin(self.handle, old_turn), STALE_TURN)
        self.assertEqual(self.dll.dsr_begin(self.handle, self.turn), OK)
        after, _ = get_result(self.dll, self.handle)
        self.assertEqual((after.turn_id, after.revision, after.input_sample_count),
                         (self.turn, before.revision, 160))
        self.assertEqual(self.dll.dsr_finish(self.handle, self.turn), OK)
        for old_turn in previous_turns:
            self.assertEqual(self.dll.dsr_begin(self.handle, old_turn), STALE_TURN)

    def test_48khz_capture_is_resampled_without_duration_change(self):
        info, _ = self.decode(self.turn, frame=1440, rate=48000)
        self.assertEqual(info.input_sample_rate, 48000)
        self.assertEqual(info.input_sample_count, len(self.fixture) * 3)

    def test_invalid_pcm_is_rejected_before_native_decode(self):
        self.assertEqual(push(self.dll, self.handle, self.turn, [0.0], 12345), INVALID_ARGUMENT)
        self.assertEqual(push(self.dll, self.handle, self.turn, [math.nan]), INVALID_ARGUMENT)
        self.assertEqual(push(self.dll, self.handle, self.turn, [math.inf]), INVALID_ARGUMENT)
        self.assertEqual(push(self.dll, self.handle, self.turn, [1.1]), INVALID_ARGUMENT)
        self.assertEqual(push(self.dll, self.handle, self.turn, [0.0] * 16001), INVALID_ARGUMENT)
        self.assertEqual(push(self.dll, self.handle, self.turn, [0.0] * 160), OK)
        self.assertEqual(push(self.dll, self.handle, self.turn, [0.0] * 480, 48000), INVALID_ARGUMENT)
        info, _ = get_result(self.dll, self.handle)
        self.assertEqual(info.input_sample_count, 160)

    def test_digital_silence_does_not_create_a_player_message(self):
        for _ in range(3):
            self.assertEqual(push(self.dll, self.handle, self.turn, [0.0] * 16000), OK)
            self.assertEqual(get_result(self.dll, self.handle)[1], "")
        self.assertEqual(self.dll.dsr_finish(self.handle, self.turn), OK)
        info, text = get_result(self.dll, self.handle)
        self.assertEqual(info.state, FINAL)
        self.assertEqual(info.input_sample_count, 48000)
        self.assertEqual(text, "")

    def test_leading_digital_silence_preserves_speech_and_capture_duration(self):
        _, original = self.decode(self.turn)
        type(self).next_turn += 1
        self.turn = type(self).next_turn
        self.assertEqual(self.dll.dsr_begin(self.handle, self.turn), OK)
        _, with_silence = self.decode(self.turn, leading_silence_seconds=2)
        self.assertEqual(original, with_silence)

    def test_small_copy_buffer_preserves_required_size_and_metadata(self):
        self.decode(self.turn, frame=6400)
        info = ResultInfo(struct_size=ctypes.sizeof(ResultInfo))
        required = ctypes.c_uint32()
        tiny = ctypes.create_string_buffer(1)
        self.assertEqual(self.dll.dsr_get_result(self.handle, ctypes.byref(info), tiny, 1, ctypes.byref(required)), BUFFER_TOO_SMALL)
        self.assertGreater(required.value, 1)
        self.assertEqual(info.state, FINAL)
        self.assertEqual(info.turn_id, self.turn)
        self.assertEqual(tiny.value, b"")
        _, copied = get_result(self.dll, self.handle)
        self.assertEqual(required.value, len(copied.encode("utf-8")) + 1)

    def test_duration_limit_rejects_whole_extra_chunk(self):
        status, handle, error = create(self.dll, make_config(max_ms=100))
        self.assertEqual(status, OK, error)
        try:
            self.assertEqual(self.dll.dsr_begin(handle, 1), OK)
            self.assertEqual(push(self.dll, handle, 1, [0.0] * 1600), OK)
            self.assertEqual(push(self.dll, handle, 1, [0.0]), LIMIT_EXCEEDED)
            info, _ = get_result(self.dll, handle)
            self.assertEqual(info.input_sample_count, 1600)
            self.assertEqual(self.dll.dsr_cancel(handle, 1), OK)
        finally:
            self.dll.dsr_destroy(handle)

    def test_invalid_configuration_and_hash_mismatch_return_errors(self):
        config = make_config()
        config.abi_version = 2
        status, handle, error = create(self.dll, config)
        self.assertEqual(status, INVALID_ARGUMENT)
        self.assertFalse(handle.value)
        self.assertTrue(error)
        config = make_config()
        config.hotwords_utf8 = "🐈".encode("utf-8")
        status, handle, _ = create(self.dll, config)
        self.assertEqual(status, INVALID_ARGUMENT)
        self.assertFalse(handle.value)
        # 同尺寸损坏文件也必须被 SHA256 拒绝，不能进入上游 ONNX 加载器
        test_root = ROOT / "native-test-temp"
        test_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=test_root) as directory:
            path = pathlib.Path(directory).resolve()
            self.assertTrue(path.is_relative_to(ROOT.resolve()))
            with (path / "encoder.int8.onnx").open("wb") as stream:
                stream.truncate(26246290)
            status, handle, error = create(self.dll, make_config(path))
            self.assertEqual(status, MODEL_ERROR)
            self.assertFalse(handle.value)
            self.assertIn("SHA256 mismatch", error)


if __name__ == "__main__":
    unittest.main(verbosity=2)
