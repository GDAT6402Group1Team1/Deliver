// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryTaskHudComponent.generated.h"

/**
 * 顶部任务条：让任务流程不用敲控制台也能玩。
 *
 * 显示的内容按状态自动切换：
 *   响铃    → 「来电：老王」/「按 J 接听」（标红）
 *   通话中  → 对方的台词
 *   待取件  → 「去取件：给独居老头送饭」/「↗ 120 米」
 *   进行中  → 「送到：独居老头家」/「↗ 80 米 · 剩余 4:32」
 *   刚送达  → 「送达！+120」停留几秒
 *
 * **挂在 PlayerController 上，不是角色上。** 骑摩托车时控制器去 Possess 了车，
 * 角色那具身体的 `IsLocallyControlled()` 变成 false——挂角色上的话任务条会在
 * 上车瞬间消失。召唤载具提示挂角色是对的（它本来就只在步行时有意义），
 * 任务条不是。
 *
 * 只读、只推 UI，不改任何游戏状态，所以纯本机跑、不需要复制。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryTaskHudComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryTaskHudComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** 关掉就不显示任务条（想自己做 UMG 界面时用）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Task HUD")
	bool bShowObjectiveBar = true;

	/** 送达之后"送达！+120"停留多久。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Task HUD",
		meta=(ClampMin="0.0", Units="s"))
	float CompletionBannerSeconds = 4.f;

	/** 剩余时间少于这个值就把计时标红。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Task HUD",
		meta=(ClampMin="0.0", Units="s"))
	float UrgentRemainingSeconds = 60.f;

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	/** 送达时记一个时间戳，用来让"送达！+120"停留几秒后自己消失。 */
	UFUNCTION()
	void HandleTaskPaid(class UDeliveryTaskDefinition* Task, const struct FDeliveryRewardBreakdown& Reward);

	/** 钱包可能比本组件晚就绪（PlayerState 是复制过来的），所以要重试订阅。 */
	void BindToWallet();

	bool PushCallState(class UDeliveryPhoneCallQueueComponent* Phone);
	bool PushCompletionBanner();
	bool PushTaskState();

	FText CompletionText;
	double CompletionShownTime = -1000.0;

	FTimerHandle BindRetryTimer;
};
