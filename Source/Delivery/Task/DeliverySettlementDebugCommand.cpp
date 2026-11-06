// Copyright Epic Games, Inc. All Rights Reserved.
//
// `Delivery.Task.Settlement` —— 不用真送一单就能看结算界面。
//
// **这条命令是必须的，不是锦上添花。** 没有它的话，调一次版式要：
// 接电话 → 跑去取货点 → 骑到收件点 → 交付，几分钟一轮；而版式是要改几十次的。
// 它还顺带回答"为什么不弹"——`SettlementWidgetClass` 留空时弹不出来是**静默**的，
// 这条命令会直接说出来。

#if !UE_BUILD_SHIPPING

#include "DeliverySettlementComponent.h"
#include "DeliverySettlementWidget.h"
#include "Data/DeliveryTaskDefinition.h"
#include "DeliveryTaskManagerComponent.h"
#include "DeliveryTaskTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

namespace
{
	void ShowTestSettlement(const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}

		APlayerController* Controller = World->GetFirstPlayerController();
		UDeliverySettlementComponent* Component = Controller
			? Controller->FindComponentByClass<UDeliverySettlementComponent>() : nullptr;
		if (!Component)
		{
			UE_LOG(LogTemp, Warning, TEXT("[结算] PlayerController 上没有 SettlementComponent。"));

			return;
		}

		if (!Component->SettlementWidgetClass)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[结算] SettlementWidgetClass 是空的——**这就是弹不出来的原因**。"
					 "在 BP_DeliveryManPC 的 Settlement 组件上填你的 WBP 类。"));

			return;
		}
		UE_LOG(LogTemp, Display, TEXT("[结算] WidgetClass = %s"),
			*Component->SettlementWidgetClass->GetName());

		// 造一份假明细。数字**刻意都不一样**：全填 100 的话，
		// 把两个控件接反了也看不出来。
		FDeliveryRewardBreakdown Reward;
		Reward.BaseReward = 120;
		Reward.TimeMultiplier = 1.25f;
		Reward.SpecialMultiplier = 1.10f;
		Reward.FinalReward = 165;
		Reward.ElapsedSeconds = 155.0f;          // 2:35，秒数带前导零才看得出格式对不对
		Reward.bOverdue = false;
		Reward.TimeGradeName = NSLOCTEXT("DeliverySettlement", "TestGrade", "准时送达");

		// 带参数 `overdue` 时测超时那一支：`TimeGradeName` 为空，
		// 控件该显示 `OverdueGradeText` 而不是留白。
		if (Args.Num() > 0 && Args[0].Equals(TEXT("overdue"), ESearchCase::IgnoreCase))
		{
			Reward.bOverdue = true;
			Reward.TimeGradeName = FText::GetEmpty();
			Reward.TimeMultiplier = 0.6f;
			Reward.FinalReward = 79;
			Reward.ElapsedSeconds = 605.0f;      // 10:05
		}

		// 随便挑一个真任务当标题；没有就传空，控件会显示空标题，不崩。
		UDeliveryTaskDefinition* Task = nullptr;
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			if (const UDeliveryTaskManagerComponent* Manager =
				GameState->FindComponentByClass<UDeliveryTaskManagerComponent>())
			{
				const TArray<UDeliveryTaskDefinition*>& Tasks = Manager->GetAllTaskDefinitions();
				if (Tasks.Num() > 0)
				{
					Task = Tasks[0];
				}
			}
		}

		Component->ShowSettlement(Task, Reward);
		UE_LOG(LogTemp, Display, TEXT("[结算] 已弹出测试结算（%s）"),
			Reward.bOverdue ? TEXT("超时") : TEXT("准时"));
	}

	FAutoConsoleCommandWithWorldAndArgs GShowSettlementCmd(
		TEXT("Delivery.Task.Settlement"),
		TEXT("弹一次测试结算界面。加参数 overdue 测超时那一支。"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ShowTestSettlement));
}

#endif
