"""在 CPU 合并 LoRA，再用固定版本 llama.cpp 导出量化模型；不启用正式模型"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys

from train_lora import MODEL_ROOT, TRAINING_ROOT, training_path, file_hash, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adapter', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--quantization', choices=['Q4_K_M', 'Q8_0'], default='Q4_K_M')
    args = parser.parse_args()
    args.adapter, args.output = training_path(args.adapter), training_path(args.output)
    converter = TRAINING_ROOT / 'converter/llama.cpp/convert_hf_to_gguf.py'
    runtime = TRAINING_ROOT.parent / 'LocalNpcModel/qwen3-1.7b/runtime'
    quantizer = runtime / 'llama-quantize.exe'
    if not args.adapter.is_dir() or not converter.is_file() or not quantizer.is_file():
        raise ValueError('缺少 LoRA、固定版本转换脚本或量化程序')
    plan = {'adapter': str(args.adapter), 'output': str(args.output), 'device': 'cpu', 'quantization': args.quantization,
            'production_configuration_changed': False, 'converter_sha256': file_hash(converter),
            'quantizer_sha256': file_hash(quantizer)}
    print(json.dumps(plan), flush=True)
    if not args.run:
        return
    args.output.mkdir(parents=True, exist_ok=False)
    plan['status'] = 'merging'
    write_json(args.output / 'export.json', plan)
    os.environ.update(CUDA_VISIBLE_DEVICES='-1', HF_HUB_OFFLINE='1', TRANSFORMERS_OFFLINE='1', TOKENIZERS_PARALLELISM='false')
    import torch
    torch.set_num_threads(8)
    from transformers import AutoModelForCausalLM, AutoTokenizer
    from peft import PeftModel
    model = AutoModelForCausalLM.from_pretrained(str(MODEL_ROOT), dtype=torch.bfloat16,
        device_map={'': 'cpu'}, local_files_only=True, trust_remote_code=False)
    model = PeftModel.from_pretrained(model, str(args.adapter), local_files_only=True).merge_and_unload(safe_merge=True)
    plan['weight_devices'] = sorted({str(parameter.device) for parameter in model.parameters()})
    if plan['weight_devices'] != ['cpu']:
        raise RuntimeError('导出只允许在 CPU 合并权重')
    merged = args.output / 'merged'
    model.save_pretrained(merged, safe_serialization=True, max_shard_size='3GB')
    AutoTokenizer.from_pretrained(str(MODEL_ROOT), local_files_only=True).save_pretrained(merged)
    shutil.copyfile(MODEL_ROOT / 'LICENSE', args.output / 'MODEL_LICENSE')
    del model
    import gc
    gc.collect()
    plan['status'] = 'merged'
    write_json(args.output / 'export.json', plan)
    bf16, quantized = args.output / 'combat-bf16.gguf', args.output / ('combat-'+args.quantization+'.gguf')
    environment = os.environ.copy()
    environment['PATH'] = str(runtime) + os.pathsep + environment.get('PATH', '')
    for name, command in (
            ('convert', [sys.executable, str(converter), str(merged), '--outfile', str(bf16), '--outtype', 'bf16']),
            ('quantize', [str(quantizer), str(bf16), str(quantized), args.quantization])):
        with (args.output / (name+'.log')).open('wb') as output:
            try:
                subprocess.run(command, env=environment, stdout=output, stderr=subprocess.STDOUT, check=True,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            except subprocess.CalledProcessError as error:
                plan.update(status='failed', failed_stage=name, command=command, exit_code=error.returncode)
                write_json(args.output / 'export.json', plan)
                raise
    plan.update(status='exported_unapproved', model_path=str(quantized), model_sha256=file_hash(quantized),
                model_bytes=quantized.stat().st_size, adapter_files={p.name: file_hash(p) for p in args.adapter.iterdir() if p.is_file()},
                cuda_initialized=torch.cuda.is_initialized())
    write_json(args.output / 'export.json', plan)
    print(json.dumps(plan), flush=True)


if __name__ == '__main__':
    main()
