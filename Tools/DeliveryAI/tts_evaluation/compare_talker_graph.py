"""显式使用 --gpu 比较当前 B 与 Talker Graph，默认只核对源码和配置"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import threading
import time

from compare_stream_runtime import (AI_ROOT, CASES, ROOT, compare_rows, existing_workers,
                                    load_voice_config, run_case, save_json)
from neural_talker_graph import checked_vendor, create_talker_graph, use_talker_graph


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', action='store_true')
    parser.add_argument('--case', choices=['short', 'all', *(case[0] for case in CASES)], default='short')
    parser.add_argument('--mode', choices=['whole', '4', '8', 'both'], default='8')
    parser.add_argument('--max-cache-tokens', type=int, default=512)
    parser.add_argument('--repeat', type=int, choices=[1, 2], default=1)
    parser.add_argument('--compare-hidden', action='store_true', help='逐步比较相同输入，额外计算不作为性能结果')
    parser.add_argument('--output', type=Path, default=ROOT / 'Saved/AIResearch/TalkerGraph' /
                        datetime.now().strftime('%Y%m%d-%H%M%S'))
    args = parser.parse_args()
    checked_vendor()
    config = load_voice_config()
    # 此脚本自行切换两条路径，环境中的正式开关不应使基线也启用 Talker Graph
    config['talker_mode'] = 'official'
    output = args.output.resolve()
    output.relative_to(ROOT / 'Saved/AIResearch')
    output.mkdir(parents=True, exist_ok=False)
    report = {'created_utc': datetime.now(timezone.utc).isoformat(), 'gpu_loaded': False,
              'implementation': 'optional_talker_forward_graph', 'production_config_changed': False,
              'same_model_instance': True, 'max_cache_tokens': args.max_cache_tokens,
              'ue_playback_measured': False, 'compare_hidden': args.compare_hidden, 'rows': [], 'capture': []}
    save_json(output / 'summary.json', report)
    if not args.gpu:
        print(json.dumps({'preflight': 'passed', 'summary': str(output / 'summary.json')}))
        return
    if workers := existing_workers():
        raise RuntimeError(f'语音进程仍在运行: {workers}')
    sys.path.insert(0, str(AI_ROOT))
    import neural_tts_worker as worker
    closed = threading.Event()
    runtime = worker.QwenVoiceEngines(config, closed)
    report['gpu_loaded'] = True
    cases = [case for case in CASES if args.case == 'all' or args.case == case[0]
             or args.case == 'short' and case[0] in ('male_short_1', 'female_short_1')]
    graphs = {}
    modes = ('whole', '8') if args.mode == 'both' else (args.mode,)
    try:
        warmup = output / 'warmup'
        warmup.mkdir()
        for voice in dict.fromkeys(case[1] for case in cases):
            engine = runtime.engines[voice]
            case = next(case for case in cases if case[1] == voice)
            run_case(worker, engine, case, modes[-1], warmup, profile=False)
            started = time.perf_counter()
            graphs[voice] = create_talker_graph(engine, args.max_cache_tokens)
            engine.synchronize()
            report['capture'].append({'voice': voice, 'elapsed_ms': (time.perf_counter() - started) * 1000})
            save_json(output / 'summary.json', report)
            graph_warmup = warmup / 'graph'
            graph_warmup.mkdir(exist_ok=True)
            with use_talker_graph(engine, graphs[voice]):
                run_case(worker, engine, case, modes[-1], graph_warmup, profile=False)
        for repeat in range(args.repeat):
            for case in cases:
              for mode in modes:
                for label in (('baseline', 'current') if repeat % 2 == 0 else ('current', 'baseline')):
                    folder = output / f'{label}-{repeat}'
                    folder.mkdir(exist_ok=True)
                    engine = runtime.engines[case[1]]
                    if label == 'current':
                        with use_talker_graph(engine, graphs[case[1]], compare_hidden=args.compare_hidden) as adapter:
                            row = run_case(worker, engine, case, mode, folder, profile=True)
                        row['graph_decode_steps'] = adapter.decode_steps
                        row['graph_prefill_tokens'] = adapter.prefill_length
                        if args.compare_hidden:
                            row['hidden_comparisons'] = adapter.hidden_comparisons
                    else:
                        row = run_case(worker, engine, case, mode, folder, profile=True)
                    row.update(variant=label, repeat=repeat,
                               implementation='predictor_and_talker_graph' if label == 'current' else 'predictor_graph')
                    report['rows'].append(row)
                    report['comparisons'] = compare_rows(report['rows'])
                    report['whole_stream_comparisons'] = []
                    for stream in report['rows']:
                        if stream['mode'] != '8':
                            continue
                        whole = next((entry for entry in report['rows'] if entry['case'] == stream['case']
                            and entry['variant'] == stream['variant'] and entry['repeat'] == stream['repeat']
                            and entry['mode'] == 'whole'), None)
                        if whole:
                            report['whole_stream_comparisons'].append(dict(case=stream['case'],
                                variant=stream['variant'], repeat=stream['repeat'],
                                same_codec=stream['codec_sha256'] == whole['codec_sha256'],
                                same_pcm=stream['pcm_sha256'] == whole['pcm_sha256']))
                    save_json(output / 'summary.json', report)
                    print(json.dumps({key: row[key] for key in ('variant', 'case', 'first_chunk_ms',
                        'complete_ms', 'generation_rtf', 'pcm_sha256', 'codec_sha256')}, ensure_ascii=False), flush=True)
        report['status'] = 'completed'
    except BaseException as error:
        report.update(status='failed', error={'type': type(error).__name__, 'message': str(error)})
        raise
    finally:
        closed.set()
        save_json(output / 'summary.json', report)
    print(json.dumps({'summary': str(output / 'summary.json')}, ensure_ascii=False))


if __name__ == '__main__':
    main()
