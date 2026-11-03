// Copyright Epic Games, Inc. All Rights Reserved.

#include "Camera/DeliveryCameraComponent.h"
#include "Camera/DeliveryCameraConfig.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/SpringArmComponent.h"

UDeliveryCameraComponent::UDeliveryCameraComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UDeliveryCameraComponent::BeginPlay()
{
    Super::BeginPlay();
    Boom = GetOwner()->FindComponentByClass<USpringArmComponent>();
    Camera = GetOwner()->FindComponentByClass<UCameraComponent>();
    if (Boom)
    {
        // 先更新距离，再由弹簧臂计算相机位置和遮挡
        Boom->AddTickPrerequisiteComponent(this);
    }
    ApplyConfig();
}

const UDeliveryCameraConfig& UDeliveryCameraComponent::GetConfig() const
{
    return Config ? *Config : *GetDefault<UDeliveryCameraConfig>();
}

// 未指定配置时保留蓝图已有的镜头设置
void UDeliveryCameraComponent::ApplyConfig()
{
    if (!Config || !Boom || !Camera)
    {
        return;
    }
    Boom->TargetArmLength = Config->Distance;
    Boom->SocketOffset = Config->Offset;
    Boom->bEnableCameraLag = Config->bSmoothPosition;
    Boom->CameraLagSpeed = Config->PositionSpeed;
    Boom->bEnableCameraRotationLag = Config->bSmoothRotation;
    Boom->CameraRotationLagSpeed = Config->RotationSpeed;
    Camera->SetFieldOfView(Config->FieldOfView);
}

void UDeliveryCameraComponent::PlayImpact()
{
    const APawn* Pawn = Cast<APawn>(GetOwner());
    if (!Pawn || !Pawn->IsLocallyControlled() || !Boom || !GetWorld())
    {
        return;
    }

    // 只在首次撞击时记录原距离，重复撞击不能把已拉远的距离当成原距离
    if (!bImpactActive)
    {
        BaseDistance = Boom->TargetArmLength;
        bImpactActive = true;
        SetComponentTickEnabled(true);
    }
    HoldUntil = GetWorld()->GetTimeSeconds() + GetConfig().ImpactHoldTime;
}

// 保持期间向外拉远，保持结束后回到撞击前的距离
void UDeliveryCameraComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                           FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!bImpactActive || !Boom || !GetWorld())
    {
        return;
    }
    const APawn* Pawn = Cast<APawn>(GetOwner());
    if (!Pawn || !Pawn->IsLocallyControlled())
    {
        // 切换控制对象后清除反馈，避免再次控制时留下拉远的镜头
        StopImpact();
        return;
    }

    const UDeliveryCameraConfig& Settings = GetConfig();
    const bool bHolding = GetWorld()->GetTimeSeconds() < HoldUntil;
    const float TargetDistance = BaseDistance + (bHolding ? Settings.ImpactDistance : 0.0f);
    const float Speed = bHolding ? Settings.ImpactOutSpeed : Settings.ImpactReturnSpeed;
    Boom->TargetArmLength = FMath::FInterpTo(Boom->TargetArmLength, TargetDistance, DeltaTime, Speed);

    if (!bHolding && FMath::IsNearlyEqual(Boom->TargetArmLength, BaseDistance, 1.0f))
    {
        StopImpact();
    }
}

// 精确恢复原距离，空闲时关闭更新
void UDeliveryCameraComponent::StopImpact()
{
    if (Boom)
    {
        Boom->TargetArmLength = BaseDistance;
    }
    bImpactActive = false;
    SetComponentTickEnabled(false);
}
