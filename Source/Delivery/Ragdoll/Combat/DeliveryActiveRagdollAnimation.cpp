// 斧头下劈：同步动作时间、采样手臂姿态，并在命中时检测可破坏的门
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Animation/AnimSequence.h"
#include "Character/DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"
#include "Task/World/DeliveryBreakableDoor.h"

// 返回源动画当前播放到的秒数；尚未开始时返回 -1
float UDeliveryActiveRagdollComponent::GetAxeSwingTime() const
{
	const UWorld* World = GetWorld();
	if (!World || AxeSwingStartTime < 0.0f)
	{
		return -1.0f;
	}

	// 所有客户端使用同一个开始时间，收到消息较晚也能跟上动作进度
	const AGameStateBase* GameState = World->GetGameState();
	const float CurrentServerTime =
	    GameState ? GameState->GetServerWorldTimeSeconds() : World->GetTimeSeconds();

	const float ElapsedSeconds = FMath::Max(0.0f, CurrentServerTime - AxeSwingStartTime);
	const float PlaybackRate = FMath::Clamp(AxeSwingPlayRate, 0.1f, 3.0f);
	return ElapsedSeconds * PlaybackRate;
}

// 返回斧头动作对手臂姿态的影响比例：0 表示不使用，1 表示完整使用
float UDeliveryActiveRagdollComponent::GetAxeSwingWeight() const
{
	if (!AxeSwingAnimation || !SwingAxe.IsValid())
	{
		return 0.0f;
	}

	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!Character || Character->IsIncapacitated())
	{
		return 0.0f;
	}

	// 倒地、起身或停止身体控制时，不再让手臂继续执行下劈
	if (!bIsActive || ReplicatedControlMode != EDeliveryRagdollControlMode::Active)
	{
		return 0.0f;
	}

	// 动作开始后可能已换掉斧头，必须确认手里仍是本次动作使用的物品
	const UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent();
	if (!Inventory || Inventory->GetHeldItem() != SwingAxe.Get())
	{
		return 0.0f;
	}

	// 抓取需要控制双手，不能同时叠加斧头动作
	const UDeliveryGrabComponent* Grab = Character->GetGrabComponent();
	if (Grab && Grab->IsGrabbing())
	{
		return 0.0f;
	}

	const float AnimationTime = GetAxeSwingTime();
	const float AnimationDuration = AxeSwingAnimation->GetPlayLength();

	// 动作开头逐渐抬高手臂影响比例，结尾逐渐降到 0，避免姿态突然切换
	// 淡入、淡出按动画秒数计算；播放速度加倍时，实际过渡时间也会减半
	return DeliveryAnimationArmPose::BlendWeight(AnimationTime, AnimationDuration, AxeSwingBlendIn,
	                                             AxeSwingBlendOut);
}

// 检查角色和手持物是否允许开始新的下劈
bool UDeliveryActiveRagdollComponent::CanStartAxeSwing() const
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!Character || Character->IsIncapacitated())
	{
		return false;
	}

	// -1 表示右手；下劈沿用右臂物理驱动的可用条件
	constexpr float RightHandSide = -1.0f;
	if (!CanDrivePunch(RightHandSide) || !AxeSwingAnimation)
	{
		return false;
	}

	if (Punch.IsActive() || Recovery.IsRising())
	{
		return false;
	}

	const UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent();
	if (!Inventory)
	{
		return false;
	}

	const ADeliveryHandheldItem* HeldItem = Inventory->GetHeldItem();
	if (!HeldItem || !HeldItem->bUsesAxeSwing)
	{
		return false;
	}

	const UDeliveryGrabComponent* Grab = Character->GetGrabComponent();
	if (Grab && Grab->IsGrabbing())
	{
		return false;
	}

	if (AxeSwingStartTime < 0.0f)
	{
		return true;
	}

	// 上一段动画结束后再留出 0.1 动画秒，避免连续下劈在结束点重叠
	constexpr float SwingRecoverySeconds = 0.1f;
	const float NextSwingAllowedTime = AxeSwingAnimation->GetPlayLength() + SwingRecoverySeconds;
	return GetAxeSwingTime() >= NextSwingAllowedTime;
}

// 通过本地状态检查后发送服务器下劈请求
void UDeliveryActiveRagdollComponent::RequestAxeSwing()
{
	if (CanStartAxeSwing())
		ServerRequestAxeSwing();
}

// 验证服务器状态与动画采样，随后广播下劈参数并安排命中检测
void UDeliveryActiveRagdollComponent::ServerRequestAxeSwing_Implementation()
{
	if (!CanStartAxeSwing())
		return;
	// 先确认动画能够提供手臂姿态，再向客户端通知挥斧
	FDeliveryAnimationArmPose Sample;
	if (!DeliveryAnimationArmPose::Sample(AxeSwingAnimation, 0, Sample))
		return;
	ADeliveryCharacter* Character = CastChecked<ADeliveryCharacter>(GetOwner());
	// 优先使用同步的服务器时钟，避免客户端接收延迟改变动作进度
	const AGameStateBase* State = GetWorld()->GetGameState();
	MulticastStartAxeSwing(Character->GetInventoryComponent()->GetHeldItem(),
	                       State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds());
	// 整段下劈窗口连续采样，低帧率也不依赖恰好一帧碰到门
	GetWorld()->GetTimerManager().SetTimer(AxeDoorHitTimer, this,
	                                       &UDeliveryActiveRagdollComponent::TryAxeDoorHit,
	                                       .025f, true, .8f / FMath::Max(AxeSwingPlayRate, .1f));
}

// 在服务器执行破门检测，仅处理仍有效的下劈动作
void UDeliveryActiveRagdollComponent::TryAxeDoorHit()
{
    const float Time = GetAxeSwingTime();
    if (!GetOwner() || !GetOwner()->HasAuthority() || GetAxeSwingWeight() <= 0.f || bAxeDoorHitConsumed || Time > 1.65f)
    {
        if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(AxeDoorHitTimer);
        return;
    }
    if (Time < .8f) return;
	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!Character || !Character->GetMesh())
		return;

	// 从胸前沿身体面朝方向检查，抬手或低头不会把扫掠起点带到地板
	const FVector Forward = GetBodyForward().GetSafeNormal2D();
    // 手臂物理在下劈末端可能低于门槛；命中体固定在身体胸前，不从地面向前扫
    const FVector Start = Character->GetMesh()->GetBoneLocation(TEXT("Hips")) + FVector(0,0,25);
	if (Forward.IsNearlyZero() || Start.IsNearlyZero())
		return;
	// 不检测角色自己和手里的斧头
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AxeDoorHit), false, Character);
	if (SwingAxe.IsValid())
		Params.AddIgnoredActor(SwingAxe.Get());
	FHitResult Hit;
	// 只取遇到的第一个物体，墙后的门不能被击中
	if (GetWorld()->SweepSingleByChannel(Hit, Start, Start + Forward * 175.f, FQuat::Identity, ECC_Visibility,
	                                     FCollisionShape::MakeSphere(65.f), Params))
	{
		// 第一个物体是可破坏的门，才执行破门
		if (ADeliveryBreakableDoor* Door = Cast<ADeliveryBreakableDoor>(Hit.GetActor()))
		{
            if (Door->ReceiveAxeHit(Character, Hit.ImpactPoint))
            {
                bAxeDoorHitConsumed = true;
                GetWorld()->GetTimerManager().ClearTimer(AxeDoorHitTimer);
            }
		}
	}
}

// 保存同步的斧头引用和服务器起始时间
void UDeliveryActiveRagdollComponent::MulticastStartAxeSwing_Implementation(ADeliveryHandheldItem* Axe,
                                                                            float ServerStartTime)
{
	SwingAxe = Axe;
	AxeSwingStartTime = ServerStartTime;
    bAxeDoorHitConsumed = false;
}

// 采样当前下劈姿态；权重为零或采样失败时返回 假
bool UDeliveryActiveRagdollComponent::SampleAxeSwing(FDeliveryAnimationArmPose& Out)
{
	Out.Weight = GetAxeSwingWeight();
	return Out.Weight > 0 && DeliveryAnimationArmPose::Sample(AxeSwingAnimation, GetAxeSwingTime(), Out);
}
