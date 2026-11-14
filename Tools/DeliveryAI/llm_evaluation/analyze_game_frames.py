"""读取 UE CSV Profiler 的逐帧 FrameTime，比较同一场景下的帧时间

每组配置单独录制一个 CSV，--label 标明 baseline、gpu99、gpu21 或 gpu14
--tag 可重复记录画面、窗口、语音和模型配置，程序不会连接游戏或模型服务
"""
import argparse
import csv
from datetime import datetime, timedelta, timezone
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys


def read_capture(path):
    """最后一行表头可能比开头更完整，按最终列顺序读取之前的逐帧数据"""
    opener = gzip.open if path.suffix.lower() == '.gz' else open
    with opener(path, 'rt', encoding='utf-8-sig', errors='replace', newline='') as stream:
        reader = csv.reader(stream)
        rows = [(reader.line_num, row) for row in reader if row and any(row)]
    headers = [(index, row) for index, (_, row) in enumerate(rows)
               if row[0].strip() == 'EVENTS' or 'FrameTime' in [cell.strip() for cell in row]]
    if not headers:
        raise ValueError('文件为空或缺少 FrameTime 列；需要 UE 导出的逐帧 CSV')
    header = [cell.strip() for cell in headers[-1][1]]
    if header.count('FrameTime') != 1:
        raise ValueError('最终表头必须有且只有一个 FrameTime 列')
    column = header.index('FrameTime')
    header_rows = {index for index, _ in headers}
    frames, missing, zero = [], [], []
    metadata, elapsed = {}, 0.0
    data_index = 0
    for index, (line, row) in enumerate(rows):
        if index in header_rows or index < headers[0][0]:
            continue
        if row[0].strip().lower() == '[hasheaderrowatend]':
            for offset in range(0, len(row)-1, 2):
                key = row[offset].strip().strip('[]').lower()
                metadata[key] = row[offset+1]
            continue
        if len(row) <= column or not row[column].strip():
            missing.append(data_index)
            data_index += 1
            continue
        try:
            duration = float(row[column])
        except ValueError as error:
            raise ValueError(f'第 {line} 行 FrameTime 不是数值') from error
        if not math.isfinite(duration) or duration < 0:
            raise ValueError(f'第 {line} 行 FrameTime 无效：{duration}')
        if duration == 0:
            zero.append(data_index)
        else:
            frames.append({'frame': data_index, 'start_seconds': elapsed, 'duration_ms': duration})
        elapsed += duration / 1000
        data_index += 1
    if not frames:
        raise ValueError('没有可用的正数 FrameTime 样本')
    complete = metadata.get('hasheaderrowatend') == '1' and len(headers) >= 2
    return frames, {'data_rows': data_index, 'valid_frames': len(frames), 'missing_frame_indices': missing,
                    'zero_frame_indices': zero, 'capture_duration_seconds': elapsed,
                    'ue_completion_footer_present': complete, 'frame_column': 'FrameTime',
                    'unit': 'ms', 'metadata': {key: value for key, value in metadata.items()
                                              if key in ('buildversion', 'platform', 'rhi', 'gpu', 'deviceprofile',
                                                         'systemresolution.resx', 'systemresolution.resy',
                                                         'endtimestamp', 'captureduration', 'csvmaxfilebytesreached')}}


def percentile(values, fraction):
    """按样本排序后线性插值，不通过平均 FPS 推算帧时间分位数"""
    ordered = sorted(values)
    position = (len(ordered)-1)*fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper]-ordered[lower])*(position-lower)


def analyze(path, label, tags=None, start_seconds=0.0, end_seconds=None, capture_start=None):
    if not label.strip():
        raise ValueError('配置标签不能为空')
    if not math.isfinite(start_seconds) or start_seconds < 0:
        raise ValueError('开始时间必须是非负秒数')
    if end_seconds is not None and (not math.isfinite(end_seconds) or end_seconds <= start_seconds):
        raise ValueError('结束时间必须晚于开始时间')
    frames, source = read_capture(path)
    selected = [frame for frame in frames if frame['start_seconds'] >= start_seconds
                and (end_seconds is None or frame['start_seconds'] < end_seconds)]
    if not selected:
        raise ValueError('选定时段内没有有效帧')
    values = [frame['duration_ms'] for frame in selected]
    first, last = selected[0], selected[-1]
    start, end = first['start_seconds'], last['start_seconds'] + last['duration_ms']/1000
    warnings = []
    if not source['ue_completion_footer_present']:
        warnings.append('没有 UE 完成标记，请确认文件已写完；普通逐帧 CSV 可能没有此标记')
    if source['missing_frame_indices'] or source['zero_frame_indices']:
        warnings.append('存在缺值或零值，未计入分位数；相对时间只累加已记录的有效帧时间')
    if len(selected) < 100:
        warnings.append('有效帧少于 100，P95 容易受少量样本影响')
    if end_seconds is not None and end_seconds > source['capture_duration_seconds']:
        warnings.append('录制长度不足请求的结束时间，不能与完整时段直接比较')
    if source['metadata'].get('csvmaxfilebytesreached') == '1':
        warnings.append('UE 已达到文件大小上限，这份录制可能不完整')
    interval = {'selection': 'frame_start_in_half_open_interval',
                'requested_start_seconds': start_seconds, 'requested_end_seconds': end_seconds,
                'first_frame_index': first['frame'], 'last_frame_index': last['frame'],
                'measured_start_seconds': start, 'measured_end_seconds': end,
                'measured_duration_seconds': sum(values)/1000,
                'capture_start_utc': None, 'estimated_start_utc': None, 'estimated_end_utc': None}
    if capture_start:
        instant = datetime.fromisoformat(capture_start.replace('Z', '+00:00'))
        if instant.tzinfo is None:
            raise ValueError('录制开始时间必须带时区，例如 2026-11-14T10:30:00Z')
        instant = instant.astimezone(timezone.utc)
        interval.update(capture_start_utc=instant.isoformat(),
                        estimated_start_utc=(instant+timedelta(seconds=start)).isoformat(),
                        estimated_end_utc=(instant+timedelta(seconds=end)).isoformat())
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else None
        if digest is None:
            hasher = hashlib.sha256()
            for block in iter(lambda: stream.read(1024*1024), b''):
                hasher.update(block)
            digest = hasher.hexdigest()
    return {'schema_version': 1, 'label': label, 'configuration': tags or {},
            'source_path': str(path.resolve()), 'source_sha256': digest,
            'analyzed_at_utc': datetime.now(timezone.utc).isoformat(), 'source': source,
            'interval': interval, 'sample_count': len(values),
            'frame_time_ms': {'median': statistics.median(values), 'p95': percentile(values, .95),
                              'p99': percentile(values, .99), 'maximum': max(values),
                              'mean': statistics.fmean(values)},
            'percentile_method': 'linear_interpolation_at_(N-1)*p',
            'frames_over_33_333ms': sum(value > 1000/30 for value in values),
            'frames_over_50ms': sum(value > 50 for value in values), 'warnings': warnings}


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--label', required=True)
    parser.add_argument('--tag', action='append', default=[], help='可重复指定 key=value，例如 gpu_layers=21')
    parser.add_argument('--from-seconds', type=float, default=0)
    parser.add_argument('--to-seconds', type=float)
    parser.add_argument('--capture-start-utc', help='从录制日志取得的开始时间，保留时区')
    parser.add_argument('--output', type=Path, help='指定时保存 JSON，否则仅输出到终端')
    args = parser.parse_args()
    try:
        tags = {}
        for tag in args.tag:
            key, separator, value = tag.partition('=')
            if not separator or not key.strip() or key in tags:
                raise ValueError('配置需要不重复的 key=value：'+tag)
            tags[key] = value
        result = analyze(args.csv, args.label, tags, args.from_seconds, args.to_seconds, args.capture_start_utc)
        text = json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False)
        if args.output:
            if args.output.resolve() == args.csv.resolve():
                raise ValueError('输出文件不能覆盖原始 CSV')
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text+'\n', encoding='utf-8')
        print(text)
    except (OSError, ValueError, csv.Error) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
