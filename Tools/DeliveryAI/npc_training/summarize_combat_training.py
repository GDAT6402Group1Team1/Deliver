"""汇总两轮战斗模型训练，保留失败结果，不执行模型或修改正式配置"""
from datetime import datetime, timezone
import hashlib
import html
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / 'Saved/AIResearch/NpcTraining'
OUT = ROOT / 'Saved/AIResearch/Implementation20261114'


def read(relative):
    return json.loads((DATA / relative).read_text(encoding='utf-8'))


def artifact(path):
    path = path.resolve()
    return {'path': str(path.relative_to(ROOT)).replace('\\', '/'),
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'bytes': path.stat().st_size}


def link(relative, label):
    # 片段由主文档引用，路径以 Document 文件夹为起点
    return f'<a href="../Saved/AIResearch/NpcTraining/{html.escape(relative)}">{html.escape(label)}</a>'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    r1, r2 = read('combat-v4/round1/run.json'), read('combat-v5/round2/run.json')
    first, second = read('combat-v4/final-acceptance-bf16-gpu99.json'), read('combat-v5/final-acceptance-q8-gpu99.json')
    frozen = DATA / 'acceptance-reviewed.json'
    frozen_sha = artifact(frozen)['sha256']
    assert frozen_sha == '77706e89104e4ec6d12704cb5b2214ff8efa9e4117cded6e8c400adf3acacced'
    assert first['source_sha256'] == second['source_sha256'] == frozen_sha
    cases = {case['id']: case for case in read('acceptance-reviewed.json')}
    train_groups = {json.loads(line)['group_id'] for line in (DATA / 'combat-v5/train.jsonl').read_text(encoding='utf-8').splitlines()}
    validation_groups = {json.loads(line)['group_id'] for line in (DATA / 'combat-v5/validation.jsonl').read_text(encoding='utf-8').splitlines()}
    acceptance_groups = {case['group_id'] for case in cases.values()}
    overlap = {'train_validation': sorted(train_groups & validation_groups),
               'train_acceptance': sorted(train_groups & acceptance_groups),
               'validation_acceptance': sorted(validation_groups & acceptance_groups)}
    assert not any(overlap.values())
    failures = []
    for result in second['results']:
        if not result['correct']:
            failures.append({'id': result['id'], 'actual': result['action'],
                             'acceptable': result['acceptable_actions'],
                             'evidence': cases[result['id']]['context']['evidence']})
    paths = [
        'combat-v4/manifest.json', 'combat-v4/situations.json', 'combat-v4/teacher.json',
        'combat-v4/student-corrections.json', 'combat-v4/training-shortcut-audit.json',
        'combat-v4/train.jsonl', 'combat-v4/validation.jsonl', 'combat-v4/round1/run.json',
        'combat-v4/round1/loss.jsonl', 'combat-v4/export-round1/export.json',
        'combat-v4/export-round1-retry/export.json',
        'combat-v4/baseline-training.json', 'combat-v4/baseline-validation.json',
        'combat-v4/round1-validation-q4.json', 'combat-v4/round1-validation-q4-greedy.json',
        'combat-v4/round1-validation-bf16-greedy.json', 'combat-v4/round1-validation-q4-imatrix-embF16.json',
        'combat-v4/round1-validation-q8.json', 'combat-v4/round1-validation-q8-greedy.json',
        'combat-v4/round1-validation-q8-embF16-greedy.json',
        'combat-v4/final-acceptance-bf16-gpu99.json',
        'combat-v5/manifest.json', 'combat-v5/situations.json', 'combat-v5/teacher.json',
        'combat-v5/train.jsonl', 'combat-v5/validation.jsonl', 'combat-v5/round2/run.json',
        'combat-v5/round2/loss.jsonl', 'combat-v5/export-round2/export.json',
        'combat-v5/round2-validation-q8-greedy.json', 'combat-v5/round2-validation-bf16-greedy.json',
        'combat-v5/final-acceptance-q8-gpu99.json', 'acceptance-reviewed.json']
    report = {
        'created_utc': datetime.now(timezone.utc).isoformat(), 'status': 'quality_gate_failed',
        'production_enabled': False, 'training_rounds': 2, 'further_training_this_iteration': False,
        'scope': {'profiles': ['chunlai', 'xiumei'], 'actions': ['Fight', 'Flee', 'Wait'],
                  'only_continuing_model_fight_or_flee': True, 'speech': False,
                  'player_dialogue_and_companions_excluded': True},
        'data': {'round1': {'independent_authored_training_stories': 120, 'log_rows': 8,
                            'log_episode_groups': 3, 'training_rows': 128, 'training_groups': 123},
                 'round2': {'additional_independent_authored_stories': 32, 'training_rows': 160,
                            'training_groups': 155}, 'internal_validation_stories': 15,
                 'acceptance_stories': 40, 'acceptance_sha256': frozen_sha,
                 'acceptance_used_for_training': False, 'acceptance_evaluation_count': 2,
                 'group_overlaps': overlap,
                 'log_running_action_was_reconstructed': True, 'full_on_policy_dagger': False,
                 'rejected_drafts_trained': False},
        'training': [r1, r2], 'round1_acceptance': first['summary'], 'round2_acceptance': second['summary'],
        'selection_before_second_acceptance': {'chosen': 'Q8_0 temperature=0',
            'basis': 'Q8 and BF16 both 14/15 on internal validation with the same failure; Q8 uses less memory',
            'model_sha256': read('combat-v5/export-round2/export.json')['model_sha256']},
        'scoring': {'reasonable_actions_minimum': .9, 'protocol_and_targets_must_be_legal': True,
            'second_scoring_definition_changed_before_inference': True,
            'unwanted_fight_is_quality_failure_not_automatically_protocol_failure': True,
            'previous_report_unchanged': True, 'technical_integration_checks_are_separate': True},
        'round2_failures': failures,
        'timing_scope': 'single model, gpu_layers=99, no UE or TTS load; not coexistence performance',
        'coexistence_99_21_14_cpu': 'pending root task; no result claimed here',
        'artifacts': [artifact(DATA / path) for path in paths],
        'implementation_artifacts': [artifact(ROOT / path) for path in [
            'Tools/DeliveryAI/combat_model.py', 'Tools/DeliveryAI/combat_model_config.json',
            'Tools/DeliveryAI/test_combat_model.py', 'Tools/DeliveryAI/llm_evaluation/start_local_model.py',
            'Tools/DeliveryAI/npc_training/train_lora.py', 'Tools/DeliveryAI/npc_training/evaluate_combat.py',
            'Tools/DeliveryAI/npc_training/build_combat_data.py', 'Tools/DeliveryAI/npc_training/build_combat_refinement.py',
            'Tools/DeliveryAI/npc_training/review_combat_data.py', 'Tools/DeliveryAI/npc_training/correct_combat_data.py',
            'Tools/DeliveryAI/npc_training/export_combat_model.py']]}
    (OUT / 'combat-training-results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    comparison = [
        ('原始 Q4', '0.7', '5 / 15', '未用于最终验收', '训练前参照；不是正式模型'),
        ('第一轮 Q4', '0.7 / 0', '12 / 15；10 / 15', '未用于最终验收', '仍会在应停手或逃跑时选择追打'),
        ('第一轮 BF16', '0', '15 / 15', '31 / 40（77.5%）', '内部样本全部通过，独立情境仍明显不足'),
        ('第二轮 Q8', '0', '14 / 15', '35 / 40（87.5%）', '未达到 90%，保持关闭'),
        ('第二轮 BF16', '0', '14 / 15', '不再重复挑选最终结果', '与 Q8 的内部选择相同，因此评测前选定 Q8')]
    table = ''.join('<tr>' + ''.join(f'<td>{html.escape(cell)}</td>' for cell in row) + '</tr>' for row in comparison)
    archive_links = ' · '.join([
        link('combat-v4/round1/run.json', '第一轮训练参数与显存'), link('combat-v4/round1/loss.jsonl', '第一轮逐步 loss'),
        link('combat-v5/round2/run.json', '第二轮训练参数与显存'), link('combat-v5/round2/loss.jsonl', '第二轮逐步 loss'),
        link('combat-v4/student-corrections.json', '教师纠错记录'), link('combat-v4/training-shortcut-audit.json', '数据偏差检查'),
        link('combat-v5/teacher.json', '第二轮教师审核'), link('combat-v5/export-round2/export.json', '导出文件与哈希'),
        link('combat-v4/final-acceptance-bf16-gpu99.json', '第一次 40 例结果'),
        link('combat-v5/final-acceptance-q8-gpu99.json', '第二次 40 例结果')])
    section = f'''<section id="combat-training-results-20261114">
<h2>战斗小模型：两轮训练与验收结果</h2>
<div class="notice"><strong>速度已达到独立试验目标，决策质量尚未通过。</strong>第二轮在 40 个封存情境中答对 35 个，合理动作比例为 87.5%，低于约定的 90%。正式配置保持关闭，不以“训练完成”代替玩法验收。</div>
<h3>训练了什么</h3>
<p>模型只接手大壮和翠花已经开始的追打、逃跑，选择继续追打、逃跑或停手。它不能另找一个目标，也不生成台词。玩家交谈、首次拱火和二狗仍交给原模型。游戏继续检查目标、距离和动作是否有效，模型没有直接造成伤害的权限。</p>
<p>第一轮有 120 个独立编写的训练故事，另加三段试玩日志整理出的八条记录，共 128 条、123 组。日志中的“正在交战”状态经过人工重建，不能算作完整的真实连续决策轨迹。15 个故事单独用于内部检查，另外 40 个独立故事封存验收。同一故事没有通过改名或改编号增加数量。</p>
<p>早期草稿把“决定停手”等答案写进输入，另有受击次数已经足够倒地却仍站立的情境，均在训练前退回重做。正式数据只保留游戏实际能够报告的台词、看到或听到的事件、挥拳和命中次数。教师模型逐例审核，再针对原始小模型的 17 个训练错误重新检查，其中三条教师答案也被修正。这是离线纠错，还没有完成由实际游戏轨迹持续收集数据的 DAgger 训练。</p>
<p>第一轮检查发现：所有人工故事都带有自己的上一句话，而且“已挨三拳、仍在追打”的训练故事几乎都被标成逃跑。这样的分布可能使模型只盯着受击次数，忽略后来发生的事。第二轮新增 32 个独立故事，补充双方和解、旧话与新动作矛盾、没有自己发言、旁人转述等情况。教师审核后，训练共 160 条、155 组。新增内容依据训练和内部检查结果编写，没有使用封存 40 例的错误详情。</p>
<h3>训练成本与模型精度</h3>
<p>两轮均在 RTX 5070 Ti Laptop 12 GB 上训练 Qwen3-1.7B，采用 BF16 LoRA，rank 16、alpha 32、批量 1、累积四步。第一轮 48 步，学习率 0.0002，训练 115.44 秒、含加载和保存共 131.29 秒；第二轮从第一轮继续训练 64 步，学习率降到 0.00005，训练 155.15 秒、总计 174.92 秒。</p>
<p>第一轮 PyTorch 峰值实际分配约 5.66 GiB、保留约 10.05 GiB；第二轮约 5.64 GiB 和 9.63 GiB。这些是训练进程的统计，不是游戏和语音同时运行时的显存。内部答案 token loss 从 0.619 降至 0.018，第二轮为 0.024；loss 只是训练误差，不能当作动作正确率。</p>
<div class="table-scroll"><table><thead><tr><th>模型</th><th>采样温度</th><th>内部检查</th><th>40 例独立验收</th><th>结论</th></tr></thead><tbody>{table}</tbody></table></div>
<p>第一轮还比较过 Q4 的训练集 imatrix 校准及嵌入矩阵保留 F16，内部结果仍是 10/15；Q8 为 14/15，BF16 为 15/15。没有把 Q4 写成通过。第二轮 Q8 与 BF16 的内部错误完全相同，因此在第二次验收前选定占空间更小的 Q8，不再用不同精度重复试到一个好看的成绩。首次导出曾缺少 sentencepiece，已在独立训练环境安装固定版本后重试，失败文件和日志仍保留。</p>
<h3>还错在哪里</h3>
<p>第二轮仍有两个明显的问题：对方说愿意赔花盆、本人已经接受解释后，模型仍选择追打；对方已经认输，本人也说“这还差不多”，模型还是没有停手。另外三例继续逃跑，其中一例已经澄清伸手只是接包，另两例则是原先为了让行或避开菜摊而移动，到了空地、对手再次冲来后仍没有改变安排。它对“局面已经变化”的理解还不够稳定。</p>
<p>第二次验收的 40 个结果全部符合动作和目标格式，没有选择新的对手；合理动作仅 35 个。评分在第二轮推理前与原要求对齐：合理动作至少 90%，目标和协议全部合法；取消后恢复旧动作等问题由独立接入测试检查。“不该追打却选择追打”保留为重要质量错误，不直接混同为协议错误。旧报告、原标签和 90% 门槛均未修改。</p>
<p>Q8 在全 GPU、4096 上下文、64 输出 token、单请求条件下，中位耗时 156 毫秒，P95 为 219 毫秒，首次请求约 391 毫秒。测量包含输入检查和完整动作输出，当时没有同时运行 UE 或语音。这只能说明独立服务响应快，尚不能说明游戏内一定流畅。99、21、14 层及 CPU 与游戏、语音共存的成本由后续记录补充。</p>
<p>本轮已做满两次训练，不再通过改验收标签、增加第三轮或挑选最终结果来宣称通过。正式配置保留原模型回退，本地服务可继续用于明确标注的成本和游戏链路试验。下一次训练需要更多实际游戏中“上一决定仍在执行，但局面已经改变”的完整记录；仍由模型作决定，不加按次数打架的规则。</p>
<p class="evidence">{archive_links} · <a href="../Saved/AIResearch/Implementation20261114/combat-training-results.json">总报告与全部文件校验值</a></p>
</section>'''
    (OUT / 'combat-training-results-section.html').write_text(section, encoding='utf-8')
    print(json.dumps({'report': str(OUT / 'combat-training-results.json'),
                      'html': str(OUT / 'combat-training-results-section.html'),
                      'status': report['status'], 'accuracy': second['summary']['accuracy']}))


if __name__ == '__main__':
    main()
