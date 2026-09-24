// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTaskDefinition.h"
#include "DeliveryTaskManagerComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GAS/DeliverGameplayTags.h"
#include "Misc/AutomationTest.h"

/**
 * 奖励结算的单元测试。
 *
 * 为什么值得单独测：EvaluateReward 是纯函数——只读任务资产，不碰组件状态、
 * 不碰网络、不碰世界，所以能在不开关卡的情况下把所有边界跑一遍。而它同时是
 * 整个任务系统里**最容易配错又最难在游戏里发现**的一块：档位顺序配反、超时边界
 * 差一秒、特殊事件重复计数，这些在 PIE 里都表现为"钱好像不太对"，靠肉眼对不出来。
 *
 * 跑法：编辑器 → Tools → Session Frontend → Automation → 筛 "Delivery.Task"。
 * 或者控制台 `Automation RunTests Delivery.Task`。
 */

namespace
{
	/** 造一份只在内存里的任务资产。档位按"剩余时间从多到少"排，和策划表一致。 */
	UDeliveryTaskDefinition* MakeTask(int32 BaseReward, float TimeLimit)
	{
		UDeliveryTaskDefinition* Task = NewObject<UDeliveryTaskDefinition>();
		Task->TaskId = TEXT("Test_Task");
		Task->BaseReward = BaseReward;
		Task->TimeLimitSeconds = TimeLimit;

		return Task;
	}

	void AddGrade(UDeliveryTaskDefinition* Task, float RemainingSeconds, float Multiplier, const TCHAR* Name)
	{
		FDeliveryTimeGrade Grade;
		Grade.RemainingSeconds = RemainingSeconds;
		Grade.Multiplier = Multiplier;
		Grade.GradeName = FText::FromString(Name);
		Task->TimeGrades.Add(Grade);
	}

	void AddEventRule(UDeliveryTaskDefinition* Task, const FGameplayTag& Tag, float Multiplier)
	{
		FDeliverySpecialEventRule Rule;
		Rule.EventTag = Tag;
		Rule.Multiplier = Multiplier;
		Task->SpecialEventRules.Add(Rule);
	}

	UDeliveryTaskManagerComponent* MakeManager()
	{
		return NewObject<UDeliveryTaskManagerComponent>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryRewardTimeGradeTest, "Delivery.Task.RewardTimeGrade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryRewardTimeGradeTest::RunTest(const FString&)
{
	UDeliveryTaskManagerComponent* Manager = MakeManager();
	UDeliveryTaskDefinition* Task = MakeTask(/*BaseReward=*/100, /*TimeLimit=*/300.f);

	// 剩余 120 秒以上算提前，0 以上算准时，超时 60 秒以内算迟到
	AddGrade(Task, 120.f, 1.5f, TEXT("提前"));
	AddGrade(Task, 0.f, 1.0f, TEXT("准时"));
	AddGrade(Task, -60.f, 0.7f, TEXT("迟到"));

	const TArray<FGameplayTag> NoEvents;

	// 用时 100 秒 → 剩 200 → 提前
	TestEqual(TEXT("提前交付吃最高档"),
		Manager->EvaluateReward(Task, 100.f, NoEvents).FinalReward, 150);

	// 用时 250 秒 → 剩 50 → 准时
	TestEqual(TEXT("剩余时间不够最高档时落到下一档"),
		Manager->EvaluateReward(Task, 250.f, NoEvents).FinalReward, 100);

	// 用时 320 秒 → 剩 -20 → 迟到
	TestEqual(TEXT("超时落到负数档"),
		Manager->EvaluateReward(Task, 320.f, NoEvents).FinalReward, 70);

	// 档位边界是闭区间：剩余正好等于阈值要吃这一档，不能掉到下一档
	TestEqual(TEXT("剩余时间正好等于档位阈值时吃这一档"),
		Manager->EvaluateReward(Task, 180.f, NoEvents).FinalReward, 150);

	// 超过最后一档之后不再继续恶化——最后一档就是表里最严厉的那级，
	// 代码不额外发明一个策划在表里看不到的倍率
	TestEqual(TEXT("超时超过最后一档仍按最后一档算，不额外惩罚"),
		Manager->EvaluateReward(Task, 5000.f, NoEvents).FinalReward, 70);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryRewardOverdueFlagTest, "Delivery.Task.RewardOverdueFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryRewardOverdueFlagTest::RunTest(const FString&)
{
	UDeliveryTaskManagerComponent* Manager = MakeManager();
	UDeliveryTaskDefinition* Task = MakeTask(100, 300.f);
	AddGrade(Task, 0.f, 1.0f, TEXT("准时"));
	AddGrade(Task, -60.f, 0.7f, TEXT("迟到"));

	const TArray<FGameplayTag> NoEvents;

	// bOverdue 的判据是"用时超过时限"，和命中哪一档是两回事。
	// 这两个概念混在一起过一次：曾经把"没匹配到档位"当成超时，
	// 结果还在时限内的交付被按超时倍率结算
	TestFalse(TEXT("正好卡在时限上不算超时"),
		Manager->EvaluateReward(Task, 300.f, NoEvents).bOverdue);
	TestTrue(TEXT("超出时限一点点就算超时"),
		Manager->EvaluateReward(Task, 300.1f, NoEvents).bOverdue);

	const FDeliveryRewardBreakdown OnTime = Manager->EvaluateReward(Task, 300.f, NoEvents);
	TestEqual(TEXT("卡点交付仍吃准时档"), OnTime.FinalReward, 100);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryRewardSpecialEventTest, "Delivery.Task.RewardSpecialEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryRewardSpecialEventTest::RunTest(const FString&)
{
	UDeliveryTaskManagerComponent* Manager = MakeManager();
	UDeliveryTaskDefinition* Task = MakeTask(100, 300.f);
	AddGrade(Task, 0.f, 1.0f, TEXT("准时"));

	// 借用两个已注册的原生 Tag 当特殊事件。这里测的是倍率怎么乘，
	// 和 Tag 的语义无关；用 RequestGameplayTag 现造的话，没注册的 Tag
	// 会返回空 Tag，两个空 Tag 互相相等，测试会假通过
	const FGameplayTag EventA = TAG_State_Stunned;
	const FGameplayTag EventB = TAG_Effect_Type_Damage;
	const FGameplayTag EventUnused = TAG_Ability_Attack_Punch_Left;

	AddEventRule(Task, EventA, 2.0f);
	AddEventRule(Task, EventB, 1.5f);

	TestEqual(TEXT("没触发任何事件时倍率为 1"),
		Manager->EvaluateReward(Task, 100.f, {}).FinalReward, 100);

	TestEqual(TEXT("单个事件按其倍率"),
		Manager->EvaluateReward(Task, 100.f, { EventA }).FinalReward, 200);

	TestEqual(TEXT("多个事件相乘"),
		Manager->EvaluateReward(Task, 100.f, { EventA, EventB }).FinalReward, 300);

	TestEqual(TEXT("没配规则的事件不影响倍率"),
		Manager->EvaluateReward(Task, 100.f, { EventUnused }).FinalReward, 100);

	// 同一个事件在列表里出现多次只计一次。沿途反复触发同一个事件
	// 把倍率叠爆是设计上明确排除的
	TestEqual(TEXT("同一个事件重复出现只计一次"),
		Manager->EvaluateReward(Task, 100.f, { EventA, EventA, EventA }).FinalReward, 200);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryRewardEdgeCaseTest, "Delivery.Task.RewardEdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryRewardEdgeCaseTest::RunTest(const FString&)
{
	UDeliveryTaskManagerComponent* Manager = MakeManager();

	// 空任务不该崩，返回全零
	const FDeliveryRewardBreakdown Empty = Manager->EvaluateReward(nullptr, 100.f, {});
	TestEqual(TEXT("空任务返回 0 奖励"), Empty.FinalReward, 0);

	// 一档都没配时倍率保持 1，按底薪原样发
	UDeliveryTaskDefinition* NoGrades = MakeTask(250, 300.f);
	const FDeliveryRewardBreakdown Plain = Manager->EvaluateReward(NoGrades, 100.f, {});
	TestEqual(TEXT("没配档位时按底薪原样发"), Plain.FinalReward, 250);
	TestEqual(TEXT("没配档位时时间倍率为 1"), Plain.TimeMultiplier, 1.0f);

	// 取整是四舍五入，不是截断。100 × 0.333 = 33.3 → 33；100 × 0.336 = 33.6 → 34
	UDeliveryTaskDefinition* Rounding = MakeTask(100, 300.f);
	AddGrade(Rounding, 0.f, 0.333f, TEXT("零头"));
	TestEqual(TEXT("向下四舍"), Manager->EvaluateReward(Rounding, 10.f, {}).FinalReward, 33);

	Rounding->TimeGrades.Empty();
	AddGrade(Rounding, 0.f, 0.336f, TEXT("零头"));
	TestEqual(TEXT("向上五入"), Manager->EvaluateReward(Rounding, 10.f, {}).FinalReward, 34);

	// 明细里的各项要如实填，UI 的结算飘字直接读它们
	UDeliveryTaskDefinition* Detailed = MakeTask(100, 300.f);
	AddGrade(Detailed, 0.f, 1.5f, TEXT("准时"));
	const FDeliveryRewardBreakdown Breakdown = Manager->EvaluateReward(Detailed, 42.f, {});
	TestEqual(TEXT("明细里的底薪"), Breakdown.BaseReward, 100);
	TestEqual(TEXT("明细里的用时"), Breakdown.ElapsedSeconds, 42.f);
	TestEqual(TEXT("明细里的档位名"), Breakdown.TimeGradeName.ToString(), FString(TEXT("准时")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
