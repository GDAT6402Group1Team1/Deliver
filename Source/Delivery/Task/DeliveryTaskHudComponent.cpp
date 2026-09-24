// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTaskHudComponent.h"

#include "DeliveryGuidanceLibrary.h"
#include "DeliveryPhoneCallQueueComponent.h"
#include "DeliveryTaskDefinition.h"
#include "DeliveryTaskManagerComponent.h"
#include "DeliveryTaskTrackerComponent.h"
#include "Economy/DeliveryWalletComponent.h"
#include "Engine/World.h"
#include "DeliveryPlayerController.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "DeliveryTaskHud"

namespace
{
	/** 钱包挂在 PlayerState 上，而 PlayerState 是复制过来的，可能比控制器晚到。 */
	constexpr float BindRetrySeconds = 0.5f;

	/** 把秒数排成 4:32。负数（超时）前面加 +，表示已经多用了这么久。 */
	FText FormatClock(float Seconds)
	{
		const bool bOver = Seconds < 0.f;
		const int32 Total = FMath::CeilToInt(FMath::Abs(Seconds));

		return FText::FromString(FString::Printf(TEXT("%s%d:%02d"),
			bOver ? TEXT("+") : TEXT(""), Total / 60, Total % 60));
	}

	/**
	 * 把相对镜头的夹角变成一个箭头。
	 *
	 * 只用八个方向而不是连续旋转的图标：这是 Slate 文本，转不了；
	 * 而玩家在跑动中其实只需要"大概往哪拐"，八向足够。
	 */
	const TCHAR* ArrowForYaw(float RelativeYaw)
	{
		const float Yaw = FRotator::NormalizeAxis(RelativeYaw);

		if (Yaw > -22.5f && Yaw <= 22.5f)   return TEXT("↑");
		if (Yaw > 22.5f && Yaw <= 67.5f)    return TEXT("↗");
		if (Yaw > 67.5f && Yaw <= 112.5f)   return TEXT("→");
		if (Yaw > 112.5f && Yaw <= 157.5f)  return TEXT("↘");
		if (Yaw > -67.5f && Yaw <= -22.5f)  return TEXT("↖");
		if (Yaw > -112.5f && Yaw <= -67.5f) return TEXT("←");
		if (Yaw > -157.5f && Yaw <= -112.5f)return TEXT("↙");

		return TEXT("↓");
	}
}

UDeliveryTaskHudComponent::UDeliveryTaskHudComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 20Hz 就够：文字每帧刷没有意义，而这条链路上有几次组件查找
	PrimaryComponentTick.TickInterval = 0.05f;
	SetIsReplicatedByDefault(false);
}

void UDeliveryTaskHudComponent::BeginPlay()
{
	Super::BeginPlay();

	BindToWallet();
}

void UDeliveryTaskHudComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BindRetryTimer);
	}

	Super::EndPlay(EndPlayReason);
}

void UDeliveryTaskHudComponent::BindToWallet()
{
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalController())
	{
		return;
	}

	UDeliveryWalletComponent* Wallet = UDeliveryWalletComponent::FindWallet(GetOwner());
	if (!Wallet)
	{
		// PlayerState 是复制过来的，可能还没到。不重试的话这个玩家整局看不到
		// 送达结算，而且没有任何报错
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				BindRetryTimer, this, &UDeliveryTaskHudComponent::BindToWallet, BindRetrySeconds, false);
		}

		return;
	}

	Wallet->OnTaskPaid.AddUniqueDynamic(this, &UDeliveryTaskHudComponent::HandleTaskPaid);
}

void UDeliveryTaskHudComponent::HandleTaskPaid(UDeliveryTaskDefinition* Task,
	const FDeliveryRewardBreakdown& Reward)
{
	const FText GradeSuffix = Reward.TimeGradeName.IsEmpty()
		? FText::GetEmpty()
		: FText::Format(LOCTEXT("GradeSuffix", "（{0}）"), Reward.TimeGradeName);

	CompletionText = FText::Format(LOCTEXT("Delivered", "送达！+{0}{1}"),
		FText::AsNumber(Reward.FinalReward), GradeSuffix);
	CompletionShownTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
}

void UDeliveryTaskHudComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bShowObjectiveBar)
	{
		return;
	}

	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalController())
	{
		return;
	}

	// 优先级：来电 > 刚送达的结算 > 当前任务。
	// 来电排最前是因为它有时限（响铃超时就算未接），玩家必须马上看到
	if (PushCallState(UDeliveryPhoneCallQueueComponent::Get(this)))
	{
		return;
	}

	if (PushCompletionBanner())
	{
		return;
	}

	PushTaskState();
}

bool UDeliveryTaskHudComponent::PushCallState(UDeliveryPhoneCallQueueComponent* Phone)
{
	UDeliveryPromptSubsystem* Prompt = GetWorld()
		? GetWorld()->GetSubsystem<UDeliveryPromptSubsystem>() : nullptr;
	if (!Phone || !Prompt)
	{
		return false;
	}

	FDeliveryPhoneCall Call;
	if (!Phone->GetCurrentCall(Call))
	{
		return false;
	}

	const FDeliveryPhoneCallContent Content = UDeliveryPhoneCallQueueComponent::GetCallContent(Call);

	switch (Phone->GetCallState())
	{
	case EDeliveryPhoneCallState::Ringing:
	{
		// 键名从**实际绑定的 FKey** 取，改了 TogglePhoneKey 提示会跟着变，
		// 不会说一套做一套（和召唤载具、切视角那两条提示同一个做法）
		const ADeliveryPlayerController* Owner = Cast<ADeliveryPlayerController>(GetOwner());
		const FText Key = (Owner && Owner->TogglePhoneKey.IsValid())
			? Owner->TogglePhoneKey.GetDisplayName()
			: LOCTEXT("PhoneKeyFallback", "手机键");
		Prompt->PushObjective(
			FText::Format(LOCTEXT("Ringing", "来电：{0}"), Content.CallerName),
			FText::Format(LOCTEXT("RingingHint", "按 {0} 接听　（{1} 秒后转未接）"),
				Key, FText::AsNumber(FMath::CeilToInt(Phone->GetStateRemainingSeconds()))),
			/*bUrgent=*/true);

		return true;
	}

	case EDeliveryPhoneCallState::InCall:
		Prompt->PushObjective(
			FText::Format(LOCTEXT("InCall", "通话中：{0}"), Content.CallerName),
			Content.Dialogue,
			/*bUrgent=*/false);

		return true;

	default:
		return false;
	}
}

bool UDeliveryTaskHudComponent::PushCompletionBanner()
{
	UWorld* World = GetWorld();
	UDeliveryPromptSubsystem* Prompt = World ? World->GetSubsystem<UDeliveryPromptSubsystem>() : nullptr;
	if (!Prompt || CompletionText.IsEmpty())
	{
		return false;
	}

	if (World->GetTimeSeconds() - CompletionShownTime > CompletionBannerSeconds)
	{
		CompletionText = FText::GetEmpty();

		return false;
	}

	Prompt->PushObjective(CompletionText, FText::GetEmpty(), /*bUrgent=*/false);

	return true;
}

bool UDeliveryTaskHudComponent::PushTaskState()
{
	UWorld* World = GetWorld();
	UDeliveryPromptSubsystem* Prompt = World ? World->GetSubsystem<UDeliveryPromptSubsystem>() : nullptr;
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Prompt || !Manager)
	{
		return false;
	}

	const APlayerState* State = Cast<APlayerController>(GetOwner())->PlayerState;
	const UDeliveryTaskTrackerComponent* Tracker =
		State ? State->FindComponentByClass<UDeliveryTaskTrackerComponent>() : nullptr;
	const UDeliveryTaskDefinition* Task = Tracker ? Tracker->GetTrackedTask() : nullptr;
	if (!Task)
	{
		return false;
	}

	const EDeliveryTaskStatus Status = Manager->GetTaskStatus(Task);
	if (Status != EDeliveryTaskStatus::AwaitingPickup && Status != EDeliveryTaskStatus::InProgress)
	{
		return false;
	}

	const bool bPickup = Status == EDeliveryTaskStatus::AwaitingPickup;
	const FText Title = bPickup
		? FText::Format(LOCTEXT("GoPickup", "去取件：{0}"), Task->DisplayName)
		: FText::Format(LOCTEXT("GoDeliver", "送达：{0}"), Task->DisplayName);

	// 明细行按有什么拼什么：地点组件还没摆时就只显示计时，不让整条消失
	TArray<FString> Parts;

	const FDeliveryGuidance Guidance = UDeliveryGuidanceLibrary::GetTaskGuidance(this, Task);
	if (Guidance.bValid)
	{
		Parts.Add(FString::Printf(TEXT("%s %s"),
			ArrowForYaw(Guidance.RelativeYaw),
			*UDeliveryGuidanceLibrary::FormatDistance(Guidance.Distance).ToString()));
	}

	bool bUrgent = false;
	if (!bPickup)
	{
		const FDeliveryTaskTimeSnapshot Time = Manager->GetTimeSnapshot(Task);
		Parts.Add(FString::Printf(TEXT("剩余 %s"), *FormatClock(Time.RemainingSeconds).ToString()));
		bUrgent = Time.bOverdue || Time.RemainingSeconds < UrgentRemainingSeconds;
	}

	Prompt->PushObjective(Title, FText::FromString(FString::Join(Parts, TEXT("　·　"))), bUrgent);

	return true;
}

#undef LOCTEXT_NAMESPACE
