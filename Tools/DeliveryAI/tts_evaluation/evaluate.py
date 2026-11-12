"""用独立环境生成中文试听，记录耗时和资源占用；只读取已经下载的模型"""
import argparse
import csv
import ctypes
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import threading
import time
import wave
import zipfile
from datetime import datetime, timezone


SENTENCES = (
    {"id": "listen", "text": "小芳，你别急，先听我把话说完。"},
    {"id": "release", "text": "大军，车先松开，咱们好好讲道理。"},
)
DEFAULT_VOICES = {"qwen": ["Uncle_Fu", "Serena"], "kokoro": ["zm_010", "zf_001"]}


def checked_path(path, root):
    path, root = Path(path).absolute(), Path(root).absolute()
    path.relative_to(root)
    for current in (path, *path.parents):
        if current.exists() or current.is_symlink():
            attributes = getattr(current.lstat(), "st_file_attributes", 0)
            if current.is_symlink() or attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise ValueError(f"试验路径不能包含链接或目录联接: {current}")
        if current == root:
            break
    path.resolve().relative_to(root.resolve())
    return path


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save_json(path, value):
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    temporary.replace(path)


def offline_environment(root, threads):
    settings = {
        "HF_HUB_OFFLINE": "1", "TRANSFORMERS_OFFLINE": "1",
        "HF_HUB_DISABLE_TELEMETRY": "1", "GRADIO_ANALYTICS_ENABLED": "False",
        "TOKENIZERS_PARALLELISM": "false", "PYTHONDONTWRITEBYTECODE": "1",
        "OMP_NUM_THREADS": str(threads), "MKL_NUM_THREADS": str(threads),
        "OPENBLAS_NUM_THREADS": str(threads),
        "HF_HOME": str(root / "cache/huggingface"),
        "TORCH_HOME": str(root / "cache/torch"), "XDG_CACHE_HOME": str(root / "cache"),
        "NUMBA_CACHE_DIR": str(root / "cache/numba"),
        "TEMP": str(root / "temp"), "TMP": str(root / "temp"),
    }
    for name, value in settings.items():
        os.environ[name] = value
    sys.dont_write_bytecode = True
    # 模型缺少文件时直接报错，评估过程不连接网络补下载
    def reject_network(event, _args):
        if event in ("socket.connect", "socket.getaddrinfo"):
            raise RuntimeError("本次试听禁止联网，请先检查安装和模型文件")
    sys.addaudithook(reject_network)


def installed_model(candidate):
    project = Path(__file__).resolve().parents[3]
    root = checked_path(project / "Saved/AIResearch/TtsEvaluation" / candidate, project)
    model = read_json(Path(__file__).with_name("models.json"))[candidate]
    installation = read_json(checked_path(root / "installation.json", project))
    if installation.get("candidate") != candidate or installation.get("model_revision") != model["revision"]:
        raise ValueError("安装记录与本次模型版本不一致")
    if not installation.get("completed_at"):
        raise ValueError("安装尚未完成")
    if Path(sys.prefix).resolve() != checked_path(root / ".venv", project).resolve():
        raise ValueError(f"请使用独立解释器: {root / '.venv/Scripts/python.exe'}")
    model_dir = checked_path(root / "model", project)
    downloads = read_json(checked_path(root / "model-downloads.json", project))
    recorded = {entry["path"]: entry for entry in downloads["files"]}
    for entry in model["files"]:
        path = checked_path(model_dir / entry["path"], project)
        if entry["path"] not in recorded or path.stat().st_size != recorded[entry["path"]]["bytes"]:
            raise ValueError(f"模型文件与下载记录不一致: {entry['path']}")
        if entry.get("bytes") and path.stat().st_size != entry["bytes"]:
            raise ValueError(f"模型文件大小不一致: {entry['path']}")
    return root, model_dir, model, installation


def voice_catalog(candidate, model_dir):
    if candidate == "qwen":
        voices = sorted(read_json(model_dir / "config.json")["talker_config"]["spk_id"])
    else:
        # 声音文件是 NumPy 压缩包，列出编号时只读取目录
        with zipfile.ZipFile(model_dir / "voices-v1.1-zh.bin") as archive:
            voices = sorted(Path(name).stem for name in archive.namelist() if name.endswith(".npy"))
    return {"voices": voices, "age_note": "声音名称或编号不保证老人听感，尚需试听", "defaults": DEFAULT_VOICES[candidate]}


def process_memory():
    if os.name != "nt":
        return {"available": False}
    class MemoryCounters(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong)] + [
            (name, ctypes.c_size_t) for name in (
                "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]
    counters = MemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
    if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
        return {"available": False, "winerror": ctypes.get_last_error()}
    return {"available": True, "rss_bytes": counters.WorkingSetSize,
            "process_peak_rss_bytes": counters.PeakWorkingSetSize, "private_bytes": counters.PrivateUsage}


class GpuSampler:
    """每秒记录一次整张显卡占用，其他程序的占用也会包含在内"""
    def __init__(self):
        self.executable = shutil.which("nvidia-smi")
        self.samples = []
        self.errors = []
        self.stop_event = threading.Event()
        self.thread = None
        self.started = time.perf_counter()

    def sample(self):
        try:
            result = subprocess.run(
                [self.executable, "--query-gpu=index,name,memory.used,memory.total,utilization.gpu,driver_version",
                 "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=3,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), check=True)
            devices = []
            for row in csv.reader(result.stdout.splitlines()):
                if len(row) == 6:
                    devices.append({"index": int(row[0]), "name": row[1].strip(),
                                    "memory_used_mib": float(row[2]), "memory_total_mib": float(row[3]),
                                    "utilization_percent": float(row[4]), "driver_version": row[5].strip()})
            self.samples.append({"elapsed_s": round(time.perf_counter() - self.started, 3), "devices": devices})
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            if not self.errors:
                self.errors.append(f"{type(error).__name__}: {error}")

    def start(self):
        if not self.executable:
            return
        self.sample()
        def poll():
            while not self.stop_event.wait(1.0):
                self.sample()
        self.thread = threading.Thread(target=poll, name="tts-evaluation-gpu", daemon=True)
        self.thread.start()

    def finish(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=4.0)
        peaks = {}
        for sample in self.samples:
            for device in sample["devices"]:
                key = str(device["index"])
                peaks[key] = max(peaks.get(key, 0), device["memory_used_mib"])
        return {"available": bool(self.samples), "interval_s": 1.0,
                "scope": "整张显卡，包含 UE 和其他进程；定时采样可能漏掉短暂峰值",
                "sampled_peak_memory_mib_by_gpu": peaks, "samples": self.samples, "errors": self.errors}


def package_versions():
    versions = {}
    for name in ("torch", "torchaudio", "qwen-tts", "transformers", "accelerate", "kokoro-onnx",
                 "misaki-fork", "onnxruntime", "numpy", "soundfile"):
        try:
            versions[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            pass
    return versions


def torch_memory(torch):
    return {"allocated_bytes": torch.cuda.memory_allocated(0), "reserved_bytes": torch.cuda.memory_reserved(0),
            "peak_allocated_bytes": torch.cuda.max_memory_allocated(0), "peak_reserved_bytes": torch.cuda.max_memory_reserved(0)}


def load_engine(candidate, model_dir, args):
    start = time.perf_counter()
    if candidate == "qwen":
        import torch
        from qwen_tts import Qwen3TTSModel
        import_ms = (time.perf_counter() - start) * 1000
        torch.set_num_threads(args.threads)
        torch.set_num_interop_threads(1)
        if not torch.cuda.is_available():
            raise RuntimeError("本次 Qwen 对照要求 CUDA；未自动改成 CPU")
        torch.cuda.set_device(0)
        torch.cuda.reset_peak_memory_stats(0)
        start = time.perf_counter()
        model = Qwen3TTSModel.from_pretrained(str(model_dir), device_map="cuda:0", dtype=torch.bfloat16,
                                            attn_implementation="sdpa", local_files_only=True)
        torch.cuda.synchronize()
        load_ms = (time.perf_counter() - start) * 1000
        info = {"device": torch.cuda.get_device_name(0), "cuda_version": torch.version.cuda,
                "attention": "sdpa", "dtype": "bfloat16", "load_memory": torch_memory(torch),
                "max_new_tokens": args.max_new_tokens, "non_streaming_mode": True,
                "random_seed_applied": True,
                "instruction": None, "instruction_note": "0.6B CustomVoice 不使用情绪指令"}
        def synthesize(text, voice, seed):
            torch.manual_seed(seed)
            torch.cuda.manual_seed_all(seed)
            torch.cuda.reset_peak_memory_stats(0)
            torch.cuda.synchronize()
            begun = time.perf_counter()
            with torch.inference_mode():
                wavs, rate = model.generate_custom_voice(text=text, language="Chinese", speaker=voice,
                                                        non_streaming_mode=True, max_new_tokens=args.max_new_tokens)
            torch.cuda.synchronize()
            elapsed = (time.perf_counter() - begun) * 1000
            if len(wavs) != 1:
                raise ValueError("一次台词应只返回一段音频")
            return wavs[0], rate, elapsed, {"torch_memory": torch_memory(torch)}
        info["generation_defaults"] = model.generate_defaults
    else:
        import onnxruntime as ort
        from kokoro_onnx import Kokoro
        from misaki import zh
        import_ms = (time.perf_counter() - start) * 1000
        options = ort.SessionOptions()
        options.intra_op_num_threads = args.threads
        options.inter_op_num_threads = 1
        options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        start = time.perf_counter()
        session = ort.InferenceSession(str(model_dir / "kokoro-v1.1-zh.onnx"), sess_options=options,
                                       providers=["CPUExecutionProvider"])
        model = Kokoro.from_session(session, str(model_dir / "voices-v1.1-zh.bin"),
                                    vocab_config=str(model_dir / "config.json"))
        g2p = zh.ZHG2P(version="1.1")
        load_ms = (time.perf_counter() - start) * 1000
        info = {"device": "CPU", "providers": session.get_providers(), "g2p": "misaki-fork ZHG2P 1.1",
                "speed": 1.0, "trim": args.trim, "random_seed_applied": False,
                "random_state_note": "未固定 ONNX 随机状态，相同台词再次合成可能产生不同样本和时长",
                "intra_op_threads": args.threads, "inter_op_threads": 1}
        def synthesize(text, voice, _seed):
            begun = time.perf_counter()
            phonemes, _ = g2p(text)
            g2p_ms = (time.perf_counter() - begun) * 1000
            if not phonemes or "❓" in phonemes:
                raise ValueError("中文转音素出现空结果或未知字，保留错误供检查")
            samples, rate = model.create(phonemes, voice=voice, speed=1.0, is_phonemes=True, trim=args.trim)
            return samples, rate, (time.perf_counter() - begun) * 1000, {"g2p_ms": g2p_ms, "phonemes": phonemes}
    return synthesize, {"import_ms": import_ms, "cold_model_load_ms": load_ms, **info}


def write_audio(path, samples, rate):
    import numpy as np
    values = np.asarray(samples, dtype=np.float32)
    if values.ndim != 1 or not values.size or not np.isfinite(values).all():
        raise ValueError("音频必须是非空、数值有效的单声道样本")
    if not 8000 <= int(rate) <= 96000:
        raise ValueError("音频采样率超出试听范围")
    peak = float(np.max(np.abs(values)))
    out_of_range = int(np.count_nonzero(np.abs(values) > 1.0))
    # 保留原采样率和音量，不对角色声音变调或添加效果
    pcm = np.rint(np.clip(values, -1.0, 1.0) * 32767).astype("<i2").tobytes()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(int(rate))
        output.writeframes(pcm)
    return {"wav": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "sample_rate": int(rate), "frames": int(values.size), "duration_s": values.size / int(rate),
            "peak_abs_before_pcm": peak, "out_of_range_samples": out_of_range,
            "rms": float(np.sqrt(np.mean(values.astype(np.float64) ** 2))), "pcm_format": "mono PCM16"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", choices=("qwen", "kokoro"), required=True)
    parser.add_argument("--list-voices", action="store_true", help="只列出已下载模型中的声音编号")
    parser.add_argument("--voices", nargs="+", help="指定最多四种声音，默认每个模型各一男一女")
    parser.add_argument("--threads", type=int, choices=range(1, 9), default=4)
    parser.add_argument("--repeats", type=int, choices=(1, 2, 3), default=2)
    parser.add_argument("--sentence-count", type=int, choices=(1, 2), default=2)
    parser.add_argument("--max-new-tokens", type=int, choices=(128, 256, 512), default=256)
    parser.add_argument("--seed", type=int, default=20261113)
    parser.add_argument("--trim", action="store_true", help="Kokoro 使用官方方法裁剪首尾静音，默认保留原始音频")
    args = parser.parse_args()
    if args.trim and args.candidate != "kokoro":
        parser.error("--trim 仅用于 Kokoro，Qwen 保留完整原始音频")
    root, model_dir, source, installation = installed_model(args.candidate)
    offline_environment(root, args.threads)
    catalog = voice_catalog(args.candidate, model_dir)
    save_json(root / "voice-catalog.json", catalog)
    if args.list_voices:
        print(json.dumps(catalog, ensure_ascii=False, indent=2))
        return 0
    voices = args.voices or DEFAULT_VOICES[args.candidate]
    if len(voices) > 4 or len({voice.lower() for voice in voices}) != len(voices):
        raise ValueError("每次比较一至四种不同声音")
    supported = {voice.lower(): voice for voice in catalog["voices"]}
    if any(voice.lower() not in supported for voice in voices):
        raise ValueError("声音编号不存在，请先运行 --list-voices")
    voices = [supported[voice.lower()] for voice in voices]
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = checked_path(root / "results" / run_id, root)
    output.mkdir(parents=True, exist_ok=False)
    result_path = output / "evaluation.json"
    result = {"schema_version": 1, "status": "started", "candidate": args.candidate, "model_source": source,
              "installation_completed_at": installation["completed_at"], "voices": voices, "sentences": SENTENCES[:args.sentence_count],
              "arguments": {**vars(args), "seed": args.seed if args.candidate == "qwen" else None},
              "python": sys.version, "platform": platform.platform(), "pid": os.getpid(),
              "packages": package_versions(), "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "rss_before_load": process_memory(), "samples": [],
              "measurement_notes": ["冷加载指本进程第一次加载模型，操作系统可能已缓存模型文件",
                                    "合成耗时从提交文本到整句音频返回，不是游戏里的首音播放延迟",
                                    "RTF 为合成秒数除以音频秒数；小于一表示此次整句合成快于音频播放",
                                    "RSS 峰值统计整个进程；Torch 峰值每次合成前重置，包含已加载模型",
                                    "相同声音与台词再次合成用于热态对照；少量样本不代表稳定的性能上限",
                                    "Qwen 设置生成长度上限防止异常长输出；当前接口不返回结束原因，句尾和漏字需要试听检查",
                                    "声音自然度、老年听感、漏字和句尾完整性尚未由脚本判断"],
              "subjective_review": "pending", "game_integration": False}
    save_json(result_path, result)
    print(f"RESULT_JSON={result_path}", flush=True)
    print(json.dumps({"candidate": args.candidate, "threads": args.threads, "trim": args.trim,
                      "seed": args.seed if args.candidate == "qwen" else None, "voices": voices},
                     ensure_ascii=False), flush=True)
    sampler = GpuSampler()
    sampler.start()
    try:
        synthesize, engine_info = load_engine(args.candidate, model_dir, args)
        result["engine"] = engine_info
        result["rss_after_load"] = process_memory()
        save_json(result_path, result)
        for repeat in range(args.repeats):
            for voice in voices:
                for index, sentence in enumerate(SENTENCES[:args.sentence_count]):
                    call_index = len(result["samples"])
                    samples, rate, elapsed_ms, extra = synthesize(sentence["text"], voice, args.seed + index)
                    audio = write_audio(output / f"{voice}_{sentence['id']}_r{repeat + 1}.wav", samples, rate)
                    row = {"call_index": call_index, "voice": voice, "sentence_id": sentence["id"],
                           "text": sentence["text"], "repeat": repeat + 1,
                           "seed": args.seed + index if args.candidate == "qwen" else None,
                           "first_synthesis_in_process": call_index == 0, "same_text_voice_repeated": repeat > 0,
                           "synthesis_ms": round(elapsed_ms, 3), "rtf": elapsed_ms / 1000 / audio["duration_s"],
                           "rss_after_synthesis": process_memory(), **audio, **extra}
                    result["samples"].append(row)
                    save_json(result_path, result)
                    print(json.dumps({key: row[key] for key in ("voice", "sentence_id", "repeat", "synthesis_ms", "duration_s", "rtf", "wav")}, ensure_ascii=False), flush=True)
        result["status"] = "completed"
    except (Exception, KeyboardInterrupt) as error:
        result["status"] = "failed"
        result["error"] = f"{type(error).__name__}: {error}"
        print(result["error"], file=sys.stderr, flush=True)
    finally:
        result["gpu_sampling"] = sampler.finish()
        result["rss_at_end"] = process_memory()
        save_json(result_path, result)
    return 0 if result["status"] == "completed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
