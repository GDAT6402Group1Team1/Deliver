

#include "Player/DeliverPlayerState.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "Economy/DeliveryWalletComponent.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"

ADeliverPlayerState::ADeliverPlayerState()
{
	// 创建 ASC
	AbilitySystemComponent =
	    CreateDefaultSubobject<UDeliverAbilitySystemComponent>(TEXT("AbilitySystemComponent"));

	// 创建 属性表
	AttributeSet = CreateDefaultSubobject<UDeliverAttributeSet>(TEXT("AttributeSet"));

	// 创建 任务追踪组件
	TaskTracker = CreateDefaultSubobject<UDeliveryTaskTrackerComponent>(TEXT("TaskTracker"));

	// 创建 钱包
	Wallet = CreateDefaultSubobject<UDeliveryWalletComponent>(TEXT("Wallet"));
}

// 组件创建完成后连接技能系统与角色切换通知
void ADeliverPlayerState::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// 服务器 Possess 和拥有客户端 OnRep_Pawn 会广播 OnPawnSet
	OnPawnSet.AddDynamic(this, &ADeliverPlayerState::HandlePawnSet);
}

// OnPawnSet，角色被控制时回调
void ADeliverPlayerState::HandlePawnSet(APlayerState* /* 未使用 */, APawn* NewPawn, APawn* /* 未使用 */)
{
	if (AbilitySystemComponent)
	{
		// 初始化技能与角色的关联
		AbilitySystemComponent->InitializeAbilityActor(this /* 技能所属的玩家状态 */, NewPawn /* 实际执行技能的角色 */);
	}
}

UAbilitySystemComponent* ADeliverPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}
