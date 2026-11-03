// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Combat/DeliveryCombatTypes.h"
#include "DeliveryRagdollCombatComponent.generated.h"

class USkeletalMeshComponent;
class UDeliveryActiveRagdollComponent;
class USoundBase;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliverPunchHitWindow, EMeleeHand, Hand);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliverPunchEnded, EMeleeHand, Hand);

/**
 * 管理一次出拳的准备、释放和结束，释放时给手臂施加冲量
 * 服务器在物理更新后记录拳头路径，GatherHits 沿该路径查询目标
 * OnPunchHitWindow 通知 GA_DeliverPunch 结算，结束或取消时关闭检测
 * 身体姿态交给 ActiveRagdoll，伤害与冷却交给能力系统
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryRagdollCombatComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    /** 由服务器多播命中音效，仅在确认命中后调用 */
    UFUNCTION(NetMulticast, Unreliable)
    void MulticastPlayPunchHit(FVector_NetQuantize10 Location);

    UDeliveryRagdollCombatComponent();

    virtual void BeginPlay() override;

    // 命中窗口更新
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    // 近战技能接口
    // 出拳控制与查询
    /** 使用身体朝向发动攻击，镜头转动不会改变拳路 */
    bool StartAttack(EMeleeHand Hand);

    // 命中目标收集
    /** 服务器查询本次命中的角色，同一角色只返回一次 */
    TArray<AActor *> GatherHits(EMeleeHand Hand) const;

    // 蓄力与出拳操作
    /**
     * 启动一次出拳并注册阶段计时器
     * @param Hand 本次出拳使用的手
     * @param AimDir 世界空间瞄准方向，内部仅使用水平分量
     * @return 身体状态和方向有效、成功启动时返回 真
     */
    bool StartPunch(EMeleeHand Hand, FVector AimDir);

    /** 取消当前出拳，清除计时器并恢复身体动作状态 */
    void CancelPunch();

    /** 返回命中检测的世界空间变换，位置为拳头骨骼沿瞄准方向偏移后的结果 */
    FTransform GetPunchTraceTransform() const;

    float GetHitRadius() const
    {
        return HitRadius;
    }

    FVector GetPunchAimDirection() const
    {
        return CachedAimDir;
    }

    bool IsPunching() const
    {
        return bIsPunching;
    }

    // 命中窗口与结束通知
    UPROPERTY(BlueprintAssignable, Category = "Punch")
    FDeliverPunchHitWindow OnPunchHitWindow;

    UPROPERTY(BlueprintAssignable, Category = "Punch")
    FDeliverPunchEnded OnPunchEnded;

  protected:
    // 出拳冲量
    /** 释放时沿瞄准方向施加的速度变化量，单位为厘米每秒 */
    UPROPERTY(EditAnywhere, Category = "Punch",
              meta = (ClampMin = "0.0", DisplayName = "出拳速度增量 (PunchImpulse)",
                      ToolTip = "释放时给手臂的起始速度增量；不是伤害。过大可能甩动身体，先检查拳路和支撑。",
                      Units = "cm/s", UIMax = "2000"))
    float PunchImpulse = 1200.f;

    /** 各部位沿同一拳路获得速度增量，保持原来的默认分配 */
    UPROPERTY(EditAnywhere, Category = "Punch|冲量分配",
              meta = (DisplayName = "手部速度倍率 (HandImpulseScale)",
                      ToolTip = "乘以出拳速度增量后作用于手部。先保持 1，再单独比较前臂与上臂的跟随。",
                      ClampMin = "0", UIMin = "0", UIMax = "2"))
    float HandImpulseScale = 1.f;

    UPROPERTY(EditAnywhere, Category = "Punch|冲量分配",
              meta = (DisplayName = "前臂速度倍率 (ForearmImpulseScale)",
                      ToolTip = "乘以出拳速度增量后作用于前臂，原默认值为 0.5。过大会牵动肩和身体。",
                      ClampMin = "0", UIMin = "0", UIMax = "1"))
    float ForearmImpulseScale = 0.5f;

    UPROPERTY(
        EditAnywhere, Category = "Punch|冲量分配",
        meta = (DisplayName = "上臂速度倍率 (UpperArmImpulseScale)",
                ToolTip = "乘以出拳速度增量后作用于上臂，原默认值为 0.1。身体被带倒时先确认这一项没有过高。",
                ClampMin = "0", UIMin = "0", UIMax = "1"))
    float UpperArmImpulseScale = 0.1f;

    // 动作时序，时间从出拳开始计算
    /** 开始出拳至释放冲量的等待时间，单位为秒 */
    UPROPERTY(EditAnywhere, Category = "Punch",
              meta = (ClampMin = "0.0", ClampMax = "0.6", DisplayName = "蓄力等待时间 (WindupDelay)",
                      ToolTip = "从开始出拳到释放的等待。实际至少 0.05 秒；需要给手臂足够时间到达蓄力姿势。",
                      Units = "s", UIMax = "0.6"))
    float WindupDelay = 0.32f;

    /** 开始出拳至触发结束事件的时间，单位为秒 */
    UPROPERTY(EditAnywhere, Category = "Punch",
              meta = (ClampMin = "0.0", DisplayName = "攻击结束时间 (PunchEndDelay)",
                      ToolTip = "从开始出拳计时。释放后持续检测至攻击结束；不直接决定手臂收回速度。",
                      Units = "s", UIMax = "2"))
    float PunchEndDelay = 0.76f;

    // 命中检测范围
    /** 检测点相对拳头骨骼沿瞄准方向的前伸距离（厘米） */
    UPROPERTY(EditAnywhere, Category = "Punch",
              meta = (ClampMin = "0.0", DisplayName = "检测中心前移距离 (ArmReachOffset)",
                      ToolTip = "检测中心从手骨骼沿拳路前移的距离；增大只扩大可命中距离，不让手臂伸得更远。",
                      Units = "cm", UIMax = "60"))
    float ArmReachOffset = 20.f;

    UPROPERTY(EditAnywhere, Category = "Punch",
              meta = (ClampMin = "0.0", DisplayName = "命中检测半径 (HitRadius)",
                      ToolTip = "拳头检测范围。先确认真实拳路与检测时间，再调整范围，避免隔空命中。",
                      Units = "cm", UIMax = "100"))
    float HitRadius = 55.f;

    // 受控骨骼
    UPROPERTY(EditAnywhere, Category = "Punch")
    FName LeftArmBone = TEXT("LeftArm");

    UPROPERTY(EditAnywhere, Category = "Punch")
    FName RightArmBone = TEXT("RightArm");

    UPROPERTY(EditAnywhere, Category = "Punch")
    FName LeftForearmBone = TEXT("LeftForeArm");

    UPROPERTY(EditAnywhere, Category = "Punch")
    FName RightForearmBone = TEXT("RightForeArm");

    UPROPERTY(EditAnywhere, Category = "Punch")
    FName LeftHandBone = TEXT("LeftHand");

    UPROPERTY(EditAnywhere, Category = "Punch")
    FName RightHandBone = TEXT("RightHand");

    // 攻击音效
    /** 左右拳分别配置的本机出拳音效 */
    UPROPERTY(EditAnywhere, Category = "Punch|Audio")
    TObjectPtr<USoundBase> LeftPunchSound;

    UPROPERTY(EditAnywhere, Category = "Punch|Audio")
    TObjectPtr<USoundBase> RightPunchSound;

    /** 服务器确认命中后播放的音效，与本机出拳音效独立配置 */
    UPROPERTY(EditAnywhere, Category = "Punch|Audio")
    TObjectPtr<USoundBase> PunchHitSound;

    // 组件缓存与本次攻击状态
    UPROPERTY(Transient)
    TObjectPtr<USkeletalMeshComponent> Mesh;

    UPROPERTY(Transient)
    TObjectPtr<UDeliveryActiveRagdollComponent> ActiveRagdoll;

    FVector CachedAimDir = FVector::ForwardVector;
    EMeleeHand ActiveHand = EMeleeHand::Left;
    bool bIsPunching = false;

    // 只有释放后的有效拳路参与检测，收拳和被打断后不再造成伤害
    bool bHitWindowOpen = false;
    FVector PreviousTraceCenter = FVector::ZeroVector;
    FVector TraceStart = FVector::ZeroVector;
    FVector TraceEnd = FVector::ZeroVector;

    FTimerHandle PunchDriveTimer;
    FTimerHandle EndTimer;

    // 定时器回调
    /** 蓄力完成后将冲量集中施加在前臂和手，形成短促、有落点的直拳 */
    void FirePunchDrive();
    void FirePunchEnded();
};
