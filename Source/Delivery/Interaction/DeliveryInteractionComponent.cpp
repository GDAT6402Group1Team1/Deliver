#include "Interaction/DeliveryInteractionComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Player/DeliveryPlayerController.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Engine/World.h"
#include "TimerManager.h"

// 读取当前目标，根据配置立即操作或开始长按
void UDeliveryInteractionComponent::BeginUse()
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character)
        return;

    if (bUseHeld)
        return;
    AActor *Target =
        Character->GetInteractProbe() ? Character->GetInteractProbe()->GetFocusedPickupActor() : nullptr;
    UDeliveryInteractableComponent *Interactable = UDeliveryInteractableComponent::FindOn(Target);
    if (!Target || !Interactable || !CanUse(Target))
        return;

    // 普通物品可以立即拾取；需要长按的目标进入后面的计时流程
    if (Interactable->HoldDuration <= KINDA_SMALL_NUMBER)
    {
        SendRequest(EDeliveryInteractionRequest::Use, Target);
        return;
    }

    // 本机计时用于显示进度，服务器会另行记录开始时间
    bUseHeld = true;
    UseHoldTarget = Target;
    UseHoldStartedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    UseHoldDuration = Interactable->HoldDuration;
    SendRequest(EDeliveryInteractionRequest::BeginHold, Target);
    GetWorld()->GetTimerManager().SetTimer(UseHoldTimer, this, &UDeliveryInteractionComponent::UpdateHold,
                                           0.02f, true);
}
// 持续检查目标和耗时，长按完成后提交一次完成请求
void UDeliveryInteractionComponent::UpdateHold()
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character)
        return;

    AActor *Target = UseHoldTarget.Get();
    // 目标改变、离开操作范围或失去操作条件时，取消本次长按
    if (!bUseHeld || !Target || !Character->GetInteractProbe() ||
        Character->GetInteractProbe()->GetFocusedPickupActor() != Target || !CanUse(Target))
    {
        CancelUse();
        return;
    }
    const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    if (Now - UseHoldStartedAt < UseHoldDuration)
        return;

    // 先停止计时并清除本机状态，避免下一次更新重复提交
    GetWorld()->GetTimerManager().ClearTimer(UseHoldTimer);
    bUseHeld = false;
    UseHoldTarget.Reset();
    UseHoldDuration = 0.0f;
    SendRequest(EDeliveryInteractionRequest::CompleteHold, Target);
}
// 停止本机长按，并按需清除服务器记录
void UDeliveryInteractionComponent::CancelUse(bool bNotifyServer)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character)
        return;

    if (!bUseHeld)
        return;
    // 清除状态前保留目标，用于取消服务器上对应的长按
    AActor *Target = UseHoldTarget.Get();
    GetWorld()->GetTimerManager().ClearTimer(UseHoldTimer);
    bUseHeld = false;
    UseHoldTarget.Reset();
    UseHoldDuration = 0.0f;
    if (bNotifyServer && Target)
        SendRequest(EDeliveryInteractionRequest::CancelHold, Target);
}
// 提供当前目标的长按进度，没有对应操作时返回 -1
float UDeliveryInteractionComponent::GetProgress(const AActor *Target) const
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character)
        return -1.0f;

    if (!bUseHeld || UseHoldTarget.Get() != Target || UseHoldDuration <= KINDA_SMALL_NUMBER)
    {
        return -1.0f;
    }
    const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    return FMath::Clamp((Now - UseHoldStartedAt) / UseHoldDuration, 0.0f, 1.0f);
}
// 先检查玩家能否操作，再询问目标是否允许执行
bool UDeliveryInteractionComponent::CanUse(const AActor *Target, bool bCheckTaskAvailability) const
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !IsValid(Target))
        return false;

    const UDeliveryInteractableComponent *Interactable = UDeliveryInteractableComponent::FindOn(Target);
    if (!Target || !Interactable || Interactable->InteractionKey != EDeliveryInteractionKey::PickupE ||
        !Interactable->CanInteract(Character))
        return false;
    if (Character->GetGrabComponent() && Character->GetGrabComponent()->IsGrabbing())
        return false;
    if (const UAbilitySystemComponent *ASC = Character->GetAbilitySystemComponent();
        ASC && ASC->HasMatchingGameplayTag(TAG_State_Stunned))
        return false;
    // 骑车时身体物理已停用，不能要求布娃娃处于可控状态，否则骑车时无法取件
    const bool bRiding = Character->IsRidingVehicle();
    if (!bRiding && Character->GetActiveRagdoll() &&
        Character->GetActiveRagdoll()->GetControlMode() != EDeliveryRagdollControlMode::Active)
        return false;

    // 条件由目标提供，增加新目标时不需要改角色交互流程
    return Interactable->CanUse.IsBound() &&
           Interactable->CanUse.Execute(Character, bCheckTaskAvailability);
}
// 供服务器复核目标所在世界、交互范围和操作条件
bool UDeliveryInteractionComponent::ValidateTarget(AActor *Target) const
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !IsValid(Target))
        return false;

    // 检查目标所在世界、交互距离和操作条件，不要求角色或镜头朝向目标
    return IsValid(Target) && Target->GetWorld() == GetWorld() && CanUse(Target);
}
// 在服务器执行通过检查的 F 键通用交互
void UDeliveryInteractionComponent::ExecuteInteract(AActor *Target)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->HasAuthority())
        return;

    // F 键只处理同一世界中的通用交互
    if (!IsValid(Target) || Target->GetWorld() != GetWorld())
        return;
    UDeliveryInteractableComponent *Interactable = UDeliveryInteractableComponent::FindOn(Target);
    if (Interactable && Interactable->InteractionKey == EDeliveryInteractionKey::GeneralF &&
        Interactable->CanInteract(Character))
    {
        Interactable->Execute(Character);
    }
}
// 在服务器执行不需要长按的物品操作
void UDeliveryInteractionComponent::ExecuteUse(AActor *Target)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    UDeliveryInteractableComponent *TargetAction = UDeliveryInteractableComponent::FindOn(Target);
    if (!Character || !Character->HasAuthority() || !TargetAction ||
        TargetAction->HoldDuration > KINDA_SMALL_NUMBER || !ValidateTarget(Target))
        return;
    // 任务物品必须长按，错误配置为零时也不能绕过服务器检查
    if (Target->FindComponentByClass<UDeliveryItemComponent>())
        return;
    if (TargetAction->Use.IsBound())
        TargetAction->Use.Execute(Character);
}
// 记录服务器收到长按开始请求的时间
void UDeliveryInteractionComponent::BeginServerHold(AActor *Target)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->HasAuthority())
        return;

    UDeliveryInteractableComponent *Interactable = UDeliveryInteractableComponent::FindOn(Target);
    if (!Interactable || Interactable->HoldDuration <= KINDA_SMALL_NUMBER || !ValidateTarget(Target))
        return;
    ServerUseHoldTarget = Target;
    ServerUseHoldStartedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}
// 核实长按时间和目标条件，通过后执行一次目标行为
void UDeliveryInteractionComponent::CompleteServerHold(AActor *Target)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->HasAuthority())
        return;

    UDeliveryInteractableComponent *Interactable = UDeliveryInteractableComponent::FindOn(Target);
    const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    // 使用服务器记录的时间检查长按时长，允许少量计时误差
    const bool bHeldLongEnough =
        Interactable && Interactable->HoldDuration > KINDA_SMALL_NUMBER &&
        Now - ServerUseHoldStartedAt >= FMath::Max(0.0f, Interactable->HoldDuration - 0.03f);
    if (ServerUseHoldTarget.Get() != Target || !bHeldLongEnough || !ValidateTarget(Target))
    {
        ServerUseHoldTarget.Reset();
        return;
    }
    // 执行操作前清除记录，避免同一次长按被重复提交
    ServerUseHoldTarget.Reset();

    if (Interactable->Use.IsBound())
        Interactable->Use.Execute(Character);
}
// 清除指定目标的长按记录，避免影响其他目标的新操作
void UDeliveryInteractionComponent::CancelServerHold(AActor *Target)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->HasAuthority())
        return;

    // 旧目标的取消请求不能清除新目标的长按记录
    if (!Target || ServerUseHoldTarget.Get() == Target)
        ServerUseHoldTarget.Reset();
}

// 把探测器选中的通用交互目标提交给服务器
void UDeliveryInteractionComponent::Interact()
{
    const ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (Character && Character->GetInteractProbe())
    {
        SendRequest(EDeliveryInteractionRequest::Interact, Character->GetInteractProbe()->GetFocusedActor());
    }
}

// 服务器直接处理请求，客户端通过玩家控制器发送
void UDeliveryInteractionComponent::SendRequest(EDeliveryInteractionRequest Request, AActor *Target)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character)
        return;
    if (Character->HasAuthority())
    {
        HandleRequest(Request, Target);
        return;
    }
    // 骑车后由车辆持有控制器，请求仍通过玩家自己的网络连接发送
    AController *Controller = Character->GetController();
    if (const ADeliveryMotorbike *Vehicle = Character->GetCurrentVehicle())
    {
        Controller = Vehicle->GetController();
    }
    if (ADeliveryPlayerController *Player = Cast<ADeliveryPlayerController>(Controller))
    {
        Player->ServerRequestInteraction(Character, Request, Target);
    }
}

// 按请求类型进入对应的服务器检查流程
void UDeliveryInteractionComponent::HandleRequest(EDeliveryInteractionRequest Request, AActor *Target)
{
    if (!GetOwner() || !GetOwner()->HasAuthority())
        return;
    switch (Request)
    {
    case EDeliveryInteractionRequest::Interact:
        ExecuteInteract(Target);
        break;
    case EDeliveryInteractionRequest::Use:
        ExecuteUse(Target);
        break;
    case EDeliveryInteractionRequest::BeginHold:
        BeginServerHold(Target);
        break;
    case EDeliveryInteractionRequest::CompleteHold:
        CompleteServerHold(Target);
        break;
    case EDeliveryInteractionRequest::CancelHold:
        CancelServerHold(Target);
        break;
    }
}

// 清理长按计时器和目标引用，防止角色销毁后继续回调
void UDeliveryInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (GetWorld())
        GetWorld()->GetTimerManager().ClearTimer(UseHoldTimer);
    UseHoldTarget.Reset();
    ServerUseHoldTarget.Reset();
    Super::EndPlay(EndPlayReason);
}
