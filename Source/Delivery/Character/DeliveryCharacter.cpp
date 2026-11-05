// 组装角色、布娃娃、战斗和交互组件，并连接输入与网络入口
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Character/DeliveryCharacter.h"
#include "Interaction/DeliveryInteractionComponent.h"
#include "Carry/DeliveryCarryComponent.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Camera/DeliveryCameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/CollisionProfile.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "AbilitySystem/Abilities/GA_DeliverPunch.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Events/DeliveryEventComponent.h"
#include "Audio/DeliveryAudioComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Delivery.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "Player/DeliverPlayerState.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "Interaction/DeliveryInteractionGeometry.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Vehicle/DeliveryVehicleSummonComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Input/DeliveryInputComponent.h"
#include "TimerManager.h"

// 创建角色组件，并设置默认输入、镜头和资源
ADeliveryCharacter::ADeliveryCharacter()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    bReplicates = true;
    SetReplicateMovement(true);
    SetNetUpdateFrequency(20.0f);
    SetMinNetUpdateFrequency(10.0f);

    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw = false;
    bUseControllerRotationRoll = false;

    // 胶囊只做镜头根和简单碰撞，不参与布娃娃物理
    CapsuleComponent = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CollisionCylinder"));
    CapsuleComponent->InitCapsuleSize(42.f, 96.0f);
    CapsuleComponent->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
    CapsuleComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    CapsuleComponent->SetSimulatePhysics(false);
    CapsuleComponent->SetEnableGravity(false);
    RootComponent = CapsuleComponent;

    Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh0"));
    Mesh->SetupAttachment(RootComponent);
    Mesh->SetRelativeLocation(FVector(0.0f, 0.0f, -96.0f));
    Mesh->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
    Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
    Mesh->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
    Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Mesh->SetSimulatePhysics(false);
    Mesh->SetEnableGravity(false);

    // 手持物品跟随右手挂点，挂点位置可直接在角色蓝图中调整
    HeldItemAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("RightHandItemAnchor"));
    HeldItemAnchor->SetupAttachment(Mesh, TEXT("RightHand"));

    CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
    CameraBoom->SetupAttachment(RootComponent);
    CameraBoom->TargetArmLength = 400.0f;
    CameraBoom->bUsePawnControlRotation = true;
    CameraBoom->bDoCollisionTest = true;

    FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
    FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
    FollowCamera->bUsePawnControlRotation = false;
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> GrabHighlightFinder(
        TEXT("/Game/Blueprint/Interaction/M_GrabHighlight"));
    if (GrabHighlightFinder.Succeeded())
    {
        FollowCamera->PostProcessSettings.WeightedBlendables.Array.Add(
            FWeightedBlendable(1.0f, GrabHighlightFinder.Object));
        FollowCamera->PostProcessBlendWeight = 1.0f;
    }

    Events = CreateDefaultSubobject<UDeliveryEventComponent>(TEXT("Events"));
    PlayerAudio = CreateDefaultSubobject<UDeliveryAudioComponent>(TEXT("PlayerAudio"));

    PlayerCamera = CreateDefaultSubobject<UDeliveryCameraComponent>(TEXT("PlayerCamera"));

    ActiveRagdoll = CreateDefaultSubobject<UDeliveryActiveRagdollComponent>(TEXT("ActiveRagdoll"));
    RagdollCombat = CreateDefaultSubobject<UDeliveryRagdollCombatComponent>(TEXT("RagdollCombat"));
    GrabComponent = CreateDefaultSubobject<UDeliveryGrabComponent>(TEXT("Grab"));
    GrabbableComponent = CreateDefaultSubobject<UDeliveryGrabbableComponent>(TEXT("Grabbable"));
    InteractProbe = CreateDefaultSubobject<UDeliveryInteractionProbeComponent>(TEXT("InteractProbe"));
    InventoryComponent = CreateDefaultSubobject<UDeliveryInventoryComponent>(TEXT("Inventory"));
    VehicleSummon = CreateDefaultSubobject<UDeliveryVehicleSummonComponent>(TEXT("VehicleSummon"));
    InteractionComponent = CreateDefaultSubobject<UDeliveryInteractionComponent>(TEXT("Interaction"));
    CarryComponent = CreateDefaultSubobject<UDeliveryCarryComponent>(TEXT("Carry"));
    PlayerInput = CreateDefaultSubobject<UDeliveryInputComponent>(TEXT("PlayerInput"));

    PunchLeftAbilityClass = UGA_DeliverPunchLeft::StaticClass();
    PunchRightAbilityClass = UGA_DeliverPunchRight::StaticClass();

    static ConstructorHelpers::FObjectFinder<UAnimSequence> StandPoseFinder(
        TEXT("/Game/Characters/TestDeliveryMan/The_Boss_Anim"));
    if (StandPoseFinder.Succeeded())
    {
        Mesh->SetAnimation(StandPoseFinder.Object);
        Mesh->SetPlayRate(0.0f);
    }
}

// 结合眩晕标记和生命值判断角色能否行动
bool ADeliveryCharacter::IsIncapacitated() const
{
    const UAbilitySystemComponent *ASC = GetAbilitySystemComponent();
    if (!ASC)
    {
        return false;
    }
    if (ASC->HasMatchingGameplayTag(TAG_State_Stunned))
    {
        return true;
    }
    // 生命值可能先于眩晕标记更新，因此还要检查生命值是否已归零
    return ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()) <= 0.0f;
}

// 在服务器确认撞击后通知被撞玩家播放镜头反馈
void ADeliveryCharacter::NotifyVehicleImpact()
{
    if (!HasAuthority())
    {
        return;
    }
    if (IsLocallyControlled())
    {
        PlayerCamera->PlayImpact();
    }
    else
    {
        ClientVehicleImpactCamera();
    }
}

void ADeliveryCharacter::ClientVehicleImpactCamera_Implementation()
{
    PlayerCamera->PlayImpact();
}

// 玩家状态到达客户端后，连接技能系统
void ADeliveryCharacter::OnRep_PlayerState()
{
    Super::OnRep_PlayerState();

    // 本机角色已在控制对象变更时连接技能组件，无需重复初始化
    if (IsLocallyControlled())
    {
        return;
    }

    // 其他玩家的角色需要等待玩家状态同步后，再连接技能组件
    ADeliverPlayerState *PS = GetPlayerState<ADeliverPlayerState>();
    if (UDeliverAbilitySystemComponent *ASC = PS ? PS->GetDeliverAbilitySystemComponent() : nullptr)
    {
        ASC->InitializeAbilityActor(PS, this);
    }
}

UAbilitySystemComponent *ADeliveryCharacter::GetAbilitySystemComponent() const
{
    const ADeliverPlayerState *PS = GetPlayerState<ADeliverPlayerState>();
    return PS ? PS->GetAbilitySystemComponent() : nullptr;
}

























// 调整控制器视角，身体转向由身体组件处理
void ADeliveryCharacter::DoLook(float Yaw, float Pitch)
{
    if (Controller)
    {
        AddControllerYawInput(Yaw);
        AddControllerPitchInput(Pitch);
    }
}

// 把移动意图交给身体组件，并发送给服务器
void ADeliveryCharacter::DoMove(float Right, float Forward)
{
    if (ActiveRagdoll)
    {
        const FVector2D Input(Right, Forward);
        ActiveRagdoll->SetMoveInput(Input);
        if (!HasAuthority())
        {
            // 本机立即响应移动，服务器也使用这次输入计算身体运动
            ServerSetMoveInput(Input, Controller ? Controller->GetControlRotation().Yaw : 0.0f);
        }
    }
}

// 本机先响应跳跃，再向服务器提交请求
void ADeliveryCharacter::DoJumpStart()
{
    if (!ActiveRagdoll)
    {
        return;
    }

    if (HasAuthority())
    {
        ServerJump();
        return;
    }

    // 客户端先本地起跳（无前摇），随后由服务端快照修正误差
    // 不在地上时 TryStartJump 自己会拒绝，这里不用额外判断
    ActiveRagdoll->TryStartJump();
    ServerJump();
}

void ADeliveryCharacter::DoJumpEnd() {}

// 持有物品时使用物品，否则请求左拳技能
void ADeliveryCharacter::DoAttackLeft()
{
    if (GrabComponent && GrabComponent->IsGrabbing())
        return;
    /**
     * 站在能就地操作的地方（摩托车后备箱）时，左键先给那个操作
     *
     * 排在手持物品之前：手上拿着快递走到车尾，这一下该是装货，
     * 不是对着车挥一下手上的东西。目标不可用时 TryActionLeft 返回假，
     * 出拳和原来完全一样——判断只有这一处。
     */
    if (InteractionComponent->TryActionLeft())
        return;
    if (InventoryComponent && InventoryComponent->HasHeldItem())
    {
        InventoryComponent->UseHeldItem();
        return;
    }
    if (UAbilitySystemComponent *ASC = GetAbilitySystemComponent())
    {
        ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(TAG_Ability_Attack_Punch_Left));
    }
}

// 允许空手攻击时请求右拳技能
void ADeliveryCharacter::DoAttackRight()
{
    if (GrabComponent && GrabComponent->IsGrabbing())
        return;
    if (InventoryComponent && InventoryComponent->HasHeldItem())
        return;
    if (UAbilitySystemComponent *ASC = GetAbilitySystemComponent())
    {
        ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(TAG_Ability_Attack_Punch_Right));
    }
}



















void ADeliveryCharacter::DoInteract()
{
    InteractionComponent->Interact();
}

void ADeliveryCharacter::DoPickup()
{
    BeginPickupInteraction();
}

void ADeliveryCharacter::DoPickupRelease()
{
    CancelPickupHold();
}

void ADeliveryCharacter::BeginPickupInteraction()
{
    InteractionComponent->BeginUse();
}

void ADeliveryCharacter::CancelPickupHold(bool bNotifyServer)
{
    InteractionComponent->CancelUse(bNotifyServer);
}

float ADeliveryCharacter::GetPickupHoldProgress(const AActor *Target) const
{
    return InteractionComponent->GetProgress(Target);
}

bool ADeliveryCharacter::CanUsePickupTarget(const AActor *Target, bool bCheckTaskAvailability) const
{
    return InteractionComponent->CanUse(Target, bCheckTaskAvailability);
}

bool ADeliveryCharacter::CanUseActionTarget(const AActor *Target) const
{
    return InteractionComponent->CanUseActionLeft(Target);
}

ADeliveryMotorbike *ADeliveryCharacter::GetCurrentVehicle() const
{
    return CarryComponent->GetVehicle();
}

ADeliveryPackageItem *ADeliveryCharacter::GetCarriedTaskPackage() const
{
    return CarryComponent->GetPackage();
}

APlayerState *ADeliveryCharacter::GetTaskPlayerState() const
{
    return CarryComponent->GetPlayerState();
}

bool ADeliveryCharacter::ReceiveTaskPackage(ADeliveryPackageItem *Package, bool bPlayFeedback)
{
    return CarryComponent->TryReceivePackage(Package, bPlayFeedback);
}

void ADeliveryCharacter::ServerInteract_Implementation(AActor *Target)
{
    InteractionComponent->ExecuteInteract(Target);
}

void ADeliveryCharacter::ServerPickup_Implementation(AActor *Target)
{
    InteractionComponent->ExecuteUse(Target);
}

void ADeliveryCharacter::ServerBeginPickupHold_Implementation(AActor *Target)
{
    InteractionComponent->BeginServerHold(Target);
}

void ADeliveryCharacter::ServerCompletePickupHold_Implementation(AActor *Target)
{
    InteractionComponent->CompleteServerHold(Target);
}

// 服务器确认成功后，只在操作玩家的客户端发布事件
void ADeliveryCharacter::ClientPickupSucceeded_Implementation()
{
    Events->Publish(EDeliveryEvent::PickupSucceeded);
}

void ADeliveryCharacter::ClientDeliverySucceeded_Implementation()
{
    Events->Publish(EDeliveryEvent::DeliverySucceeded);
}

void ADeliveryCharacter::ServerCancelPickupHold_Implementation(AActor *Target)
{
    InteractionComponent->CancelServerHold(Target);
}

bool ADeliveryCharacter::StartMeleeAttack(EMeleeHand Hand)
{
    return RagdollCombat && RagdollCombat->StartAttack(Hand);
}

TArray<AActor *> ADeliveryCharacter::GatherMeleeHits(EMeleeHand Hand) const
{
    return RagdollCombat ? RagdollCombat->GatherHits(Hand) : TArray<AActor *>();
}

void ADeliveryCharacter::EndMeleeAttack(EMeleeHand /* 未使用 */) {}

bool ADeliveryCharacter::IsMeleeAttacking() const
{
    return RagdollCombat && RagdollCombat->IsPunching();
}

// 过滤无效输入，再更新服务器上的朝向和移动意图
void ADeliveryCharacter::ServerSetMoveInput_Implementation(FVector2D Input, float AimYaw)
{
    if (Controller && FMath::IsFinite(AimYaw))
    {
        FRotator ControlRotation = Controller->GetControlRotation();
        ControlRotation.Yaw = FMath::UnwindDegrees(AimYaw);
        Controller->SetControlRotation(ControlRotation);
    }

    if (ActiveRagdoll)
    {
        // 拒绝无效数值，并将每个输入轴限制在正常范围内
        const float Right = FMath::IsFinite(Input.X) ? FMath::Clamp(Input.X, -1.0f, 1.0f) : 0.0f;
        const float Forward = FMath::IsFinite(Input.Y) ? FMath::Clamp(Input.Y, -1.0f, 1.0f) : 0.0f;
        ActiveRagdoll->SetMoveInput(FVector2D(Right, Forward));
    }
}

// 限制连续起跳请求，只记录实际成功的起跳
void ADeliveryCharacter::ServerJump_Implementation()
{
    UWorld *World = GetWorld();
    if (!ActiveRagdoll || !World)
    {
        return;
    }

    const float Now = World->GetTimeSeconds();
    if (Now - LastServerJumpTime < MinimumJumpInterval)
    {
        return;
    }

    // 只记录成功起跳的时间，被拒绝的请求不延长下一次起跳的等待时间
    if (ActiveRagdoll->TryStartJump())
    {
        LastServerJumpTime = Now;
    }
}

// 角色只提供输入组件的接入点，具体绑定由输入组件完成
void ADeliveryCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    PlayerInput->Bind(PlayerInputComponent);
}

bool ADeliveryCharacter::IsGrabChordHeld() const
{
    return PlayerInput && PlayerInput->IsGrabChordHeld();
}

void ADeliveryCharacter::UnPossessed()
{
    PlayerInput->ResetInput();
    Super::UnPossessed();
}

void ADeliveryCharacter::OnRep_Controller()
{
    Super::OnRep_Controller();
    if (!IsLocallyControlled()) PlayerInput->ResetInput();
}
