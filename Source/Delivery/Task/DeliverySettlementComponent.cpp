// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliverySettlementComponent.h"

#include "Blueprint/UserWidget.h"
#include "Player/DeliveryPlayerController.h"
#include "DeliverySettlementWidget.h"
#include "DeliveryTaskTypes.h"
#include "Economy/DeliveryWalletComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "TimerManager.h"

namespace
{
	/**
	 * 订阅不上时的重试间隔。
	 *
	 * **这个常量不能叫 `BindRetrySeconds`**：钱包那边的匿名命名空间里已经有一个同名的，
	 * Unity Build 把多个 cpp 合并成一个翻译单元之后会重名。CLAUDE.md 记过同一个坑
	 * （任务 HUD 的 `TaskHudBindRetrySeconds` 就是为此改的名）。
	 */
	constexpr float SettlementBindRetrySeconds = 0.5f;

	/** 面板 ID。光标归 `SetUIPanelOpen` 按集合统一管，见组件注释。 */
	const FName SettlementPanelId(TEXT("Settlement"));
}

UDeliverySettlementComponent::UDeliverySettlementComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UDeliverySettlementComponent::BeginPlay()
{
	Super::BeginPlay();

	// 只有本机才弹。远端玩家的 PlayerController 上也有这个组件，
	// 不判的话服务器那边会给每个连接都造一个 widget。
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalController())
	{
		return;
	}

	BindToWallet();
}

void UDeliverySettlementComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BindRetryTimer);
		World->GetTimerManager().ClearTimer(AutoHideTimer);
	}
	// 退出时也要把面板注销掉，否则光标状态会留在"还有面板开着"。
	HideSettlement();

	if (UDeliveryWalletComponent* Wallet = FindOwningWallet())
	{
		Wallet->OnTaskPaid.RemoveDynamic(this, &UDeliverySettlementComponent::HandleTaskPaid);
	}

	Super::EndPlay(EndPlayReason);
}

UDeliveryWalletComponent* UDeliverySettlementComponent::FindOwningWallet() const
{
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	APlayerState* State = Controller ? Controller->PlayerState : nullptr;

	return UDeliveryWalletComponent::FindWallet(State);
}

void UDeliverySettlementComponent::BindToWallet()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UDeliveryWalletComponent* Wallet = FindOwningWallet();
	if (!Wallet)
	{
		/**
		 * **拿不到就重试，不能放弃。**
		 *
		 * 钱包挂在 PlayerState 上，而 PlayerState 在客户端是复制过来的——
		 * PlayerController 的 `BeginPlay` 跑的时候它常常还是空的。
		 * 不重试的话这个玩家**整局都不会弹结算，而且一条报错都没有**。
		 * 钱包自己订阅 GameState 时用的也是这一招，同一个理由。
		 */
		World->GetTimerManager().SetTimer(BindRetryTimer, this,
			&UDeliverySettlementComponent::BindToWallet, SettlementBindRetrySeconds, false);

		return;
	}

	World->GetTimerManager().ClearTimer(BindRetryTimer);
	Wallet->OnTaskPaid.AddUniqueDynamic(this, &UDeliverySettlementComponent::HandleTaskPaid);
}

void UDeliverySettlementComponent::HandleTaskPaid(UDeliveryTaskDefinition* Task,
	const FDeliveryRewardBreakdown& Reward)
{
	ShowSettlement(Task, Reward);
}

void UDeliverySettlementComponent::ShowSettlement(UDeliveryTaskDefinition* Task,
	const FDeliveryRewardBreakdown& Reward)
{
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	UWorld* World = GetWorld();
	if (!Controller || !Controller->IsLocalController() || !World)
	{
		return;
	}

	if (!SettlementWidgetClass)
	{
		// 静默不弹是最难查的那种——所以这里必须出声。
		UE_LOG(LogTemp, Warning,
			TEXT("[结算] SettlementWidgetClass 没填，不弹结算界面。"
				 "在 BP_DeliveryManPC 的 SettlementComponent 上填上你的 WBP。"));

		return;
	}

	// 连着交付两单时，先把上一张收干净再开新的。
	// 不收的话会叠两层 widget，而面板集合里只登记了一次、关一次就全没了，
	// 剩下那张永远留在屏幕上。
	HideSettlement();

	ActiveWidget = CreateWidget<UDeliverySettlementWidget>(Controller, SettlementWidgetClass);
	if (!ActiveWidget)
	{
		return;
	}

	ActiveWidget->AddToViewport(ZOrder);

	// 光标归 C++ 统一管：面板集合一变，`SetUIPanelOpen` 自己去设
	// `bShowMouseCursor` 和输入模式。这里只负责报告"我开了一个面板"。
	if (ADeliveryPlayerController* DeliveryController = Cast<ADeliveryPlayerController>(Controller))
	{
		DeliveryController->SetUIPanelOpen(SettlementPanelId, true);
	}

	ActiveWidget->OnRevealFinished.AddUObject(this, &UDeliverySettlementComponent::HandleRevealFinished);
	ActiveWidget->ApplyReward(Task, Reward);
}

void UDeliverySettlementComponent::HandleRevealFinished(UDeliverySettlementWidget* Widget)
{
	// 只接受当前面板的完成通知，旧面板不能启动新面板的关闭计时器
	UWorld* World = GetWorld();
	if (Widget && Widget == ActiveWidget && World && AutoHideSeconds > 0.f)
	{
		World->GetTimerManager().SetTimer(AutoHideTimer, this,
			&UDeliverySettlementComponent::HideSettlement, AutoHideSeconds, false);
	}
}

void UDeliverySettlementComponent::HideSettlement()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AutoHideTimer);
	}

	if (ActiveWidget)
	{
		ActiveWidget->OnRevealFinished.RemoveAll(this);
		ActiveWidget->CancelReveal();
		ActiveWidget->RemoveFromParent();
		ActiveWidget = nullptr;
	}

	// **注销必须和移除写在一起。** 分开写的话总有一条退出路径会漏掉其中一个，
	// 而漏掉注销的表现是"面板没了、光标还在"——玩家只会看到视角转不了。
	if (ADeliveryPlayerController* DeliveryController = Cast<ADeliveryPlayerController>(GetOwner()))
	{
		DeliveryController->SetUIPanelOpen(SettlementPanelId, false);
	}
}
