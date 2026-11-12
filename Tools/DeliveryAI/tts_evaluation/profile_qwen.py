"""拆分 B 音色的生成耗时，只使用已经安装的本地模型"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import platform
import sys
import time

from evaluate import (
    GpuSampler, SENTENCES, checked_path, installed_model, offline_environment,
    package_versions, process_memory, save_json, torch_memory, write_audio,
)


@contextmanager
def measure_stages(model, synchronize):
    """在整段生成和解码前后等待 CUDA 完成，内部预测只记录调用耗时"""
    stats = {"generate_calls": 0, "generate_ms": 0.0,
             "tokenizer_decode_calls": 0, "tokenizer_decode_ms": 0.0,
             "code_predictor_calls": 0, "code_predictor_cpu_wall_ms": 0.0,
             "audio_code_shapes": []}
    predictor = model.model.talker.code_predictor
    tokenizer = model.model.speech_tokenizer
    saved = []

    def replace(owner, name, wrapper):
        had_instance_value = name in vars(owner)
        instance_value = vars(owner).get(name)
        original = getattr(owner, name)
        saved.append((owner, name, had_instance_value, instance_value))
        setattr(owner, name, wrapper(original))

    def timed_stage(name):
        def wrap(original):
            def call(*args, **kwargs):
                synchronize()
                started = time.perf_counter()
                stats[name + "_calls"] += 1
                try:
                    result = original(*args, **kwargs)
                    if name == "generate":
                        stats["audio_code_shapes"] = [list(codes.shape) for codes in result[0]]
                    return result
                finally:
                    synchronize()
                    stats[name + "_ms"] += (time.perf_counter() - started) * 1000
            return call
        return wrap

    def timed_predictor(original):
        def call(*args, **kwargs):
            started = time.perf_counter()
            stats["code_predictor_calls"] += 1
            try:
                return original(*args, **kwargs)
            finally:
                # 此处不增加 CUDA 等待，累计时间包含函数内部原有的同步和 CPU 工作
                stats["code_predictor_cpu_wall_ms"] += (time.perf_counter() - started) * 1000
        return call

    try:
        replace(model.model, "generate", timed_stage("generate"))
        replace(tokenizer, "decode", timed_stage("tokenizer_decode"))
        replace(predictor, "generate", timed_predictor)
        yield stats
    finally:
        for owner, name, had_instance_value, instance_value in reversed(saved):
            if had_instance_value:
                setattr(owner, name, instance_value)
            else:
                delattr(owner, name)


def module_details(module):
    if module is None:
        return {"available": False}
    parameter = next(module.parameters(), None)
    return {"available": True, "class": type(module).__name__,
            "attention_config": getattr(getattr(module, "config", None), "_attn_implementation", None),
            "parameter_device": str(parameter.device) if parameter is not None else None,
            "parameter_dtype": str(parameter.dtype) if parameter is not None else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--warmup", type=int, choices=(0, 1), default=1,
                        help="先用相同台词合成一次，再记录正式结果；默认开启")
    args = parser.parse_args()
    root, model_dir, source, installation = installed_model("qwen")
    offline_environment(root, threads=4)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = checked_path(root / "profiles" / run_id, root)
    output.mkdir(parents=True, exist_ok=False)
    result_path = output / "profile.json"
    result = {
        "schema_version": 1, "status": "started", "candidate": "qwen",
        "model_source": source, "installation_completed_at": installation["completed_at"],
        "text": SENTENCES[0]["text"], "voice": "Uncle_Fu", "seed": 20261113,
        "settings": {"non_streaming_mode": True, "attention": "sdpa", "dtype": "bfloat16",
                     "device": "cuda:0", "max_new_tokens": 256, "threads": 4,
                     "interop_threads": 1, "warmup_calls": args.warmup, "measured_calls": 1},
        "python": sys.version, "platform": platform.platform(), "pid": os.getpid(),
        "packages": package_versions(), "samples": [],
        "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "measurement_notes": [
            "generate 和 tokenizer.decode 在调用前后等待 CUDA 完成，分别记录整段生成和解码耗时",
            "code_predictor_cpu_wall_ms 是内部函数的累计调用时间，未在每次调用前后增加 CUDA 等待",
            "code_predictor 时间包含在 generate 时间内，不能相加，也不能当作独立 GPU 耗时",
            "模块的 attention_config 只说明配置，不能证明实际选择了哪一种 SDPA 内核",
            "正式结果只有一次，预热和分段计时会影响耗时，不能据此推断稳定性能上限",
            "声音参数沿用 B 试听，音频保持原采样率；自然度、漏字和句尾需要试听检查",
        ],
        "game_integration": False,
    }
    save_json(result_path, result)
    print(f"RESULT_JSON={result_path}", flush=True)
    sampler = GpuSampler()
    sampler.start()
    try:
        started = time.perf_counter()
        import torch
        from qwen_tts import Qwen3TTSModel
        result["import_ms"] = (time.perf_counter() - started) * 1000
        torch.set_num_threads(4)
        torch.set_num_interop_threads(1)
        if not torch.cuda.is_available():
            raise RuntimeError("本次测量要求 CUDA，没有自动切换到 CPU")
        torch.cuda.set_device(0)
        torch.cuda.reset_peak_memory_stats(0)
        started = time.perf_counter()
        model = Qwen3TTSModel.from_pretrained(
            str(model_dir), device_map="cuda:0", dtype=torch.bfloat16,
            attn_implementation="sdpa", local_files_only=True)
        torch.cuda.synchronize()
        result["model_load_ms"] = (time.perf_counter() - started) * 1000
        result["generation_defaults"] = model.generate_defaults
        result["device"] = torch.cuda.get_device_name(0)
        result["cuda_version"] = torch.version.cuda
        result["load_memory"] = torch_memory(torch)
        decoder = getattr(model.model.speech_tokenizer.model, "decoder", None)
        result["modules"] = {
            "talker": module_details(model.model.talker),
            "code_predictor": module_details(model.model.talker.code_predictor),
            "decoder_transformer": module_details(getattr(decoder, "pre_transformer", None)),
        }
        save_json(result_path, result)
        for phase in (["warmup"] if args.warmup else []) + ["measured"]:
            torch.manual_seed(result["seed"])
            torch.cuda.manual_seed_all(result["seed"])
            torch.cuda.reset_peak_memory_stats(0)
            torch.cuda.synchronize()
            with measure_stages(model, torch.cuda.synchronize) as stages:
                started = time.perf_counter()
                with torch.inference_mode():
                    wavs, rate = model.generate_custom_voice(
                        text=result["text"], language="Chinese", speaker="Uncle_Fu",
                        non_streaming_mode=True, max_new_tokens=256)
                torch.cuda.synchronize()
                total_ms = (time.perf_counter() - started) * 1000
            if len(wavs) != 1:
                raise ValueError("本次台词应返回一段音频")
            audio = write_audio(output / f"Uncle_Fu_{phase}.wav", wavs[0], rate)
            row = {"phase": phase, "total_synthesis_ms": total_ms, **stages, **audio,
                   "rtf": total_ms / 1000 / audio["duration_s"],
                   "torch_memory": torch_memory(torch), "process_memory": process_memory()}
            result["samples"].append(row)
            save_json(result_path, result)
            print(f"{phase}: total_ms={total_ms:.3f}, generate_ms={stages['generate_ms']:.3f}, "
                  f"decode_ms={stages['tokenizer_decode_ms']:.3f}, "
                  f"predictor_calls={stages['code_predictor_calls']}, wav={audio['wav']}", flush=True)
        result["status"] = "completed"
    except (Exception, KeyboardInterrupt) as error:
        result["status"] = "failed"
        result["error"] = f"{type(error).__name__}: {error}"
        print(result["error"], file=sys.stderr, flush=True)
    finally:
        result["gpu_sampling"] = sampler.finish()
        result["process_memory_at_end"] = process_memory()
        save_json(result_path, result)
    return 0 if result["status"] == "completed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
