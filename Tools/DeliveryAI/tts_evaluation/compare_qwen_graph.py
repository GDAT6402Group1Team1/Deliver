"""用同一条 Uncle_Fu 台词比较原版和内部预测 CUDA Graph，只修改本进程实例"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import importlib
import os
from pathlib import Path
import platform
import sys
import threading
import time
from types import SimpleNamespace

from evaluate import (
    GpuSampler, SENTENCES, checked_path, installed_model, offline_environment,
    package_versions, process_memory, read_json, save_json, torch_memory, write_audio,
)
from profile_qwen import measure_stages, module_details


VENDOR_COMMIT = "a70afc0f81f7f5f8801c3227968f1102f43f211c"
VENDOR_FILES = {
    "predictor_graph.py": (8664, "9f74155040fb6ac2381375331f76fb4efcd5b3c0761ebfc4977717223145cb76"),
    "sampling.py": (2358, "d52f136cb5d985f532239cb3f74ce5e8b9bfd269080e99295f09ce5f87bac988"),
    "LICENSE": (1073, "442472a518bf71e371f2581aa0fcaf6ee2ef6854f78c340fdbe87c099950ea82"),
    "__init__.py": (0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
}
PREDICTOR_ARGUMENTS = {
    "max_new_tokens": 15, "do_sample": True, "top_p": 1.0, "top_k": 50,
    "temperature": 0.9, "output_hidden_states": True, "return_dict_in_generate": True,
}


def checked_vendor(root):
    directory = checked_path(root / "vendor/faster_qwen_v032", root)
    manifest = read_json(checked_path(directory / "source-manifest.json", root))
    if manifest.get("commit") != VENDOR_COMMIT or manifest.get("license") != "MIT":
        raise ValueError("优化源码的版本或许可与本次试验不符")
    recorded = {entry["path"]: entry for entry in manifest["files"]}
    for name, (size, digest) in VENDOR_FILES.items():
        data = checked_path(directory / name, root).read_bytes()
        if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"优化源码校验失败: {name}")
        if recorded[name]["bytes"] != size or recorded[name]["sha256"] != digest:
            raise ValueError(f"优化源码记录不一致: {name}")
    return directory, manifest


@contextmanager
def fp32_graph_sampling(module):
    original = module.sample_logits

    def sample(logits, **kwargs):
        expected = {"temperature": 0.9, "top_k": 50, "top_p": 1.0, "do_sample": True}
        if kwargs != expected:
            raise ValueError("本次 Graph 只比较 B 试听原有的采样设置")
        # Transformers 在采样前转为 float32，这里保持相同的计算精度
        return original(logits.float(), **kwargs)

    module.sample_logits = sample
    try:
        yield
    finally:
        module.sample_logits = original


@contextmanager
def use_predictor_graph(predictor, graph, *, hidden_size, device, dtype, output_dtype):
    """仅替换当前实例的内部预测，退出试验或发生异常时恢复原方法"""
    had_instance_value = "generate" in vars(predictor)
    instance_value = vars(predictor).get("generate")
    owner_thread = threading.get_ident()
    busy = threading.Lock()

    def generate(*args, **kwargs):
        if args or set(kwargs) != {"inputs_embeds", *PREDICTOR_ARGUMENTS}:
            raise ValueError("内部预测接口发生变化，停止本次 Graph 对照")
        for key, value in PREDICTOR_ARGUMENTS.items():
            actual = kwargs[key]
            if actual != value or (isinstance(value, bool) and type(actual) is not bool):
                raise ValueError(f"内部预测参数与 B 试听不一致: {key}")
        embeds = kwargs["inputs_embeds"]
        if tuple(embeds.shape) != (1, 2, hidden_size) or embeds.dtype != dtype or embeds.device != device:
            raise ValueError("Graph 只接受当前 CUDA 设备上的单条 bfloat16 输入")
        if threading.get_ident() != owner_thread or not busy.acquire(blocking=False):
            raise RuntimeError("本次 Graph 只允许创建它的线程依次调用")
        try:
            tokens = graph.run(embeds)
            if tuple(tokens.shape) != (15,) or tokens.dtype != output_dtype or tokens.device != device:
                raise ValueError("Graph 返回的声音编码形状、类型或设备不正确")
            # 原 Talker 只读取 sequences，后续文本、结束判断和声音解码继续使用原实现
            return SimpleNamespace(sequences=tokens.unsqueeze(0))
        finally:
            busy.release()

    predictor.generate = generate
    try:
        yield
    finally:
        if had_instance_value:
            predictor.generate = instance_value
        else:
            delattr(predictor, "generate")


def synthesize_once(model, torch, output, name):
    torch.manual_seed(20261113)
    torch.cuda.manual_seed_all(20261113)
    torch.cuda.reset_peak_memory_stats(0)
    torch.cuda.synchronize()
    with measure_stages(model, torch.cuda.synchronize) as stages:
        started = time.perf_counter()
        with torch.inference_mode():
            wavs, rate = model.generate_custom_voice(
                text=SENTENCES[0]["text"], language="Chinese", speaker="Uncle_Fu",
                non_streaming_mode=True, max_new_tokens=256)
        torch.cuda.synchronize()
        elapsed_ms = (time.perf_counter() - started) * 1000
    if len(wavs) != 1:
        raise ValueError("本次台词应返回一段音频")
    audio = write_audio(output / f"Uncle_Fu_{name}.wav", wavs[0], rate)
    return {"name": name, "total_synthesis_ms": elapsed_ms, **stages, **audio,
            "rtf": elapsed_ms / 1000 / audio["duration_s"],
            "torch_memory": torch_memory(torch), "process_memory": process_memory(),
            "audio_trimmed": False, "pcm_clipping": "仅在转换 PCM16 时限制到 [-1, 1]；越界样本数另行记录"}


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    root, model_dir, source, installation = installed_model("qwen")
    vendor, manifest = checked_vendor(root)
    offline_environment(root, threads=4)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = checked_path(root / "graph-comparisons" / run_id, root)
    output.mkdir(parents=True, exist_ok=False)
    result_path = output / "comparison.json"
    result = {
        "schema_version": 1, "status": "started", "model_source": source, "vendor": manifest,
        "installation_completed_at": installation["completed_at"],
        "text": SENTENCES[0]["text"], "voice": "Uncle_Fu", "seed": 20261113,
        "order": ["baseline_1", "graph_1", "graph_2", "baseline_2"], "samples": [],
        "settings": {"non_streaming_mode": True, "attention": "sdpa", "dtype": "bfloat16",
                     "sampling_dtype": "float32", "device": "cuda:0", "max_new_tokens": 256,
                     "threads": 4, "interop_threads": 1, "predictor": PREDICTOR_ARGUMENTS},
        "python": sys.version, "platform": platform.platform(), "pid": os.getpid(),
        "packages": package_versions(), "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "measurement_notes": [
            "原版预热一次；录图时间单独记录；正式顺序为原版、Graph、Graph、原版",
            "每条台词重新设置相同种子，录图预热消耗的随机数不带入正式合成",
            "仅替换内部预测，保留原文本构造、Talker 生成、结束判断和声音解码",
            "generate 和 tokenizer.decode 在边界等待 CUDA 完成；内部预测调用不额外同步",
            "内部预测调用时间包含在 generate 内，不能相加或当作独立 GPU 耗时",
            "显存差值包含 Graph 缓存与工作区；原版正式测量时 Graph 也仍驻留显存",
            "音频按原采样率保存，不裁剪首尾或变调；记录 PCM16 转换前越界样本数",
            "相同参数和种子不能保证不同计算路径逐样本相同，音色、漏字和句尾需要试听",
            "每条结果只有一次测量，不与先前不同 GPU 负荷下的结果直接计算收益",
        ],
        "subjective_review": "pending", "game_integration": False,
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
            raise RuntimeError("本次对照要求 CUDA，没有自动切换到 CPU")
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
        predictor = model.model.talker.code_predictor
        result["modules"] = {"talker": module_details(model.model.talker),
                             "code_predictor": module_details(predictor)}
        if model.model.config.talker_config.num_code_groups != 16:
            raise ValueError("本次 Graph 只核对了 16 组声音编码")
        result["warmup"] = synthesize_once(model, torch, output, "baseline_warmup")
        save_json(result_path, result)

        sys.path.insert(0, str(vendor.parent))
        try:
            graph_module = importlib.import_module("faster_qwen_v032.predictor_graph")
        finally:
            sys.path.remove(str(vendor.parent))
        if Path(graph_module.__file__).resolve() != (vendor / "predictor_graph.py").resolve():
            raise ValueError("导入的 Graph 模块不属于已校验的试验目录")
        with fp32_graph_sampling(graph_module):
            torch.cuda.synchronize()
            before = torch_memory(torch)
            torch.cuda.reset_peak_memory_stats(0)
            started = time.perf_counter()
            graph = graph_module.PredictorGraph(
                predictor, predictor.model.config, model.model.config.talker_config.hidden_size,
                device="cuda:0", dtype=torch.bfloat16, do_sample=True, top_k=50, top_p=1.0, temperature=0.9)
            graph.capture(num_warmup=3)
            torch.cuda.synchronize()
            after = torch_memory(torch)
            result["graph_capture"] = {
                "initialization_and_capture_ms": (time.perf_counter() - started) * 1000,
                "configured_warmup_runs": 3, "capture_stream_warmup_runs": 1,
                "recorded_loop_runs": 1, "before": before, "after": after,
                "extra_allocated_bytes": after["allocated_bytes"] - before["allocated_bytes"],
                "extra_reserved_bytes": after["reserved_bytes"] - before["reserved_bytes"],
            }
            save_json(result_path, result)
            for name in result["order"]:
                if name.startswith("graph"):
                    with use_predictor_graph(
                        predictor, graph, hidden_size=model.model.config.talker_config.hidden_size,
                        device=torch.device("cuda:0"), dtype=torch.bfloat16, output_dtype=torch.long):
                        row = synthesize_once(model, torch, output, name)
                else:
                    row = synthesize_once(model, torch, output, name)
                result["samples"].append(row)
                save_json(result_path, result)
                print(f"{name}: total_ms={row['total_synthesis_ms']:.3f}, "
                      f"generate_ms={row['generate_ms']:.3f}, decode_ms={row['tokenizer_decode_ms']:.3f}, "
                      f"predictor_calls={row['code_predictor_calls']}, wav={row['wav']}", flush=True)
        result["status"] = "completed"
    except (Exception, KeyboardInterrupt) as error:
        result["status"] = "cancelled" if isinstance(error, KeyboardInterrupt) else "failed"
        result["error"] = f"{type(error).__name__}: {error}"
        print(result["error"], file=sys.stderr, flush=True)
    finally:
        result["gpu_sampling"] = sampler.finish()
        result["process_memory_at_end"] = process_memory()
        save_json(result_path, result)
    return 0 if result["status"] == "completed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
