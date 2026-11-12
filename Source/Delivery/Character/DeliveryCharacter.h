// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "Combat/DeliveryCombatInterface.h"
#include "GameFramework/Pawn.h"
#include "DeliveryCharacter.generated.h"

// 交互与携带组件
class UDeliveryInteractionComponent;
class UDeliveryCarryComponent;
class UAbilitySystemComponent;
class UGameplayEffect;

// 身体与镜头组件
class UCapsuleComponent;
class UDeliveryActiveRagdollComponent;
class UDeliveryRagdollCombatComponent;
class UDeliveryGrabComponent;
class UDeliveryGrabbableComponent;
class UDeliveryInteractionProbeComponent;

// 物品与车辆组件
class UDeliveryInventoryComponent;
class UDeliveryVehicleSummonComponent;
class UDeliveryTrunkPassengerComponent;
class ADeliveryMotorbike;
class ADeliveryPackageItem;
class APlayerState;
class USkeletalMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UGameplayAbility;
class USceneComponent;
class UDeliveryEventComponent;
class UDeliveryAudioComponent;
class UDeliveryInputComponent;
class UDeliveryCameraComponent;

/** 组装角色组件，提供移动、战斗、抓取和快递交付的操作入口 */
UCLASS()

class ADeliveryCharacter : public APawn, public IAbilitySystemInterface, public IDeliveryCombatInterface
{
    GENERATED_BODY()

    // 输入组件
    // 输入处理
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UDeliveryInputComponent> PlayerInput;

    // 本地事件与提示音
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UDeliveryEventComponent> Events;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UDeliveryAudioComponent> PlayerAudio;

    // 角色组件
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryInteractionComponent> InteractionComponent;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryCarryComponent> CarryComponent;

    /** 角色根组件，供镜头跟随并参与简单碰撞；身体运动由骨骼物理处理 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UCapsuleComponent> CapsuleComponent;

    /** 角色的骨骼网格，布娃娃组件通过它控制身体各部位 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USkeletalMeshComponent> Mesh;

    /** 控制镜头距离，并在遇到遮挡时缩短距离 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USpringArmComponent> CameraBoom;

    /** 玩家使用的第三人称镜头 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UCameraComponent> FollowCamera;

    /** 应用镜头配置，处理本机撞击反馈 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UDeliveryCameraComponent> PlayerCamera;

    // 物理身体与抓取组件
    /** 控制身体站立、行走、跳跃和倒地恢复 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryActiveRagdollComponent> ActiveRagdoll;

    /** 控制出拳动作和物理受击 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryRagdollCombatComponent> RagdollCombat;

    /** 处理角色主动抓取物体和松手 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryGrabComponent> GrabComponent;

    /** 允许其他角色抓取这名角色 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryGrabbableComponent> GrabbableComponent;

    /** 查找附近可交互的物体，并在本机显示操作提示 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryInteractionProbeComponent> InteractProbe;

    /** 管理手持物品、快捷栏和物品切换 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryInventoryComponent> InventoryComponent;

    /** 召唤摩托车并保存冷却状态，按键提示由提示组件更新 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryVehicleSummonComponent> VehicleSummon;

public:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
    TObjectPtr<UDeliveryTrunkPassengerComponent> TrunkPassenger;
private:

    /** 手持物品的右手挂点，可在角色蓝图中调整位置和朝向 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USceneComponent> HeldItemAnchor;

  protected:

    // 跳跃限制
    // 起跳限制
    /** 服务器接受两次起跳的最短间隔，单位为秒；用于限制重复请求，实际能否起跳仍由布娃娃组件判断 */
    UPROPERTY(EditAnywhere, Category = "Ragdoll|Network", meta = (ClampMin = "0.0"))
    float MinimumJumpInterval = 0.05f;

    // 技能与生命配置
    // 技能与生命值
    // 持续恢复生命值的效果，在角色蓝图中配置
    UPROPERTY(EditDefaultsOnly, Category = "Ability")
    TSubclassOf<UGameplayEffect> HealthRegenEffect;

    // 伤害效果，在角色蓝图中配置；伤害数值由攻击时传入
    UPROPERTY(EditDefaultsOnly, Category = "Ability")
    TSubclassOf<UGameplayEffect> DamageEffect;

    // 左拳技能，默认使用代码中的实现，也可指定技能蓝图
    UPROPERTY(EditDefaultsOnly, Category = "Ability")
    TSubclassOf<UGameplayAbility> PunchLeftAbilityClass;

    /** 右拳技能，可在角色蓝图中替换 */
    UPROPERTY(EditDefaultsOnly, Category = "Ability")
    TSubclassOf<UGameplayAbility> PunchRightAbilityClass;

    /** 苏醒所需的生命值百分比；数值越高，晕倒后等待回血的时间越长；被拖拽时仍会正常回血 */
    UPROPERTY(EditDefaultsOnly, Category = "Ability", meta = (ClampMin = "1.0", ClampMax = "100.0"))
    float StunRecoverHealthPercent = 50.0f;

  public:
    ADeliveryCharacter();
    virtual void UnPossessed() override;
    virtual void OnRep_Controller() override;
    /** 由服务器通知被撞玩家播放镜头反馈 */
    void NotifyVehicleImpact();

    /** 在被撞玩家的客户端拉远镜头 */
    UFUNCTION(Client, Reliable)
    void ClientVehicleImpactCamera();

    /** 通知操作玩家取件成功，由本机监听者响应 */
    UFUNCTION(Client, Reliable)
    void ClientPickupSucceeded();

    /** 通知操作玩家交付成功，由本机监听者响应 */
    UFUNCTION(Client, Reliable)
    void ClientDeliverySucceeded();

    // 玩家状态同步到客户端后，连接角色与技能组件
    virtual void OnRep_PlayerState() override;

    /** 取得玩家状态上的技能组件；玩家状态尚未就绪时返回空指针 */
    virtual UAbilitySystemComponent *GetAbilitySystemComponent() const override;

  protected:
    // 输入绑定与按键回调
    virtual void SetupPlayerInputComponent(class UInputComponent *PlayerInputComponent) override;

  public:

    // 移动与视角接口
    /** 接收左右、前后输入，交给布娃娃组件处理，并向服务器发送输入 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoMove(float Right, float Forward);

    /** 调整镜头的水平和垂直朝向 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoLook(float Yaw, float Pitch);

    /** 请求起跳；客户端先响应，再由服务器校验 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoJumpStart();

    /** 保留松开跳跃键的接口，当前不需要额外处理 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoJumpEnd();

    /** 左键攻击；手中有物品时使用物品，空手时发动左拳 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoAttackLeft();

    /** 右键攻击；抓取中或手中有物品时不发动右拳 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoAttackRight();

    // 交互接口
    /** 向服务器请求与当前选中的物体交互，由服务器检查并执行 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoInteract();

    /** 开始拾取操作，目标需要长按时启动计时 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoPickup();

    /** 松开拾取键时取消长按；骑车时也必须转发松开操作，避免服务器保留上一次长按记录 */
    UFUNCTION(BlueprintCallable, Category = "Input")
    virtual void DoPickupRelease();

    /** 返回指定目标的长按进度，范围为 0 到 1；当前没有长按该目标时返回 -1 */
    float GetPickupHoldProgress(const AActor *Target) const;
    /** 检查目标和角色是否满足拾取或交付条件；可选择跳过任务可用性检查 */
    bool CanUsePickupTarget(const AActor *Target, bool bCheckTaskAvailability = true) const;

    /** 左键就地操作目标现在能不能用（摩托车后备箱装卸货） */
    UFUNCTION(BlueprintPure, Category = "Interaction")
    bool CanUseActionTarget(const AActor *Target) const;

    // 任务快递

    /** 服务器接收任务快递；步行时放到手上，骑车时放到车上；接收失败时返回假 */
    UFUNCTION(BlueprintCallable, Category = "Delivery|Carry")
    bool ReceiveTaskPackage(ADeliveryPackageItem *Package, bool bPlayFeedback = true);

    /** 取得当前携带的任务快递，包括手上的快递和车上的快递；没有时返回空指针 */
    UFUNCTION(BlueprintPure, Category = "Delivery|Carry")
    ADeliveryPackageItem *GetCarriedTaskPackage() const;

    /** 取得用于任务结算的玩家状态；骑车后控制器改为控制车辆，因此需要从车辆取得玩家状态 */
    UFUNCTION(BlueprintPure, Category = "Delivery|Carry")
    APlayerState *GetTaskPlayerState() const;

    /** 取得角色正在驾驶的车辆；根据角色的附着关系和车辆驾驶员判断，没有驾驶车辆时返回空指针 */
    UFUNCTION(BlueprintPure, Category = "Delivery|Carry")
    ADeliveryMotorbike *GetCurrentVehicle() const;

    /** 判断角色是否正在驾驶车辆 */
    UFUNCTION(BlueprintPure, Category = "Delivery|Carry")

    bool IsRidingVehicle() const
    {
        return GetCurrentVehicle() != nullptr;
    }

    // 近战接口：由技能调用，具体出拳动作交给战斗组件
    virtual bool StartMeleeAttack(EMeleeHand Hand) override;
    virtual TArray<AActor *> GatherMeleeHits(EMeleeHand Hand) const override;
    virtual void EndMeleeAttack(EMeleeHand Hand) override;
    virtual bool IsMeleeAttacking() const override;

    // 服务器请求
    /** 服务器接收移动输入和镜头水平角度，过滤无效数值 */
    UFUNCTION(Server, Unreliable)
    void ServerSetMoveInput(FVector2D Input, float AimYaw);

    /** 服务器检查请求间隔，再尝试起跳 */
    UFUNCTION(Server, Reliable)
    void ServerJump();

    /** 服务器检查目标是否可交互，通过后执行交互 */
    UFUNCTION(Server, Reliable)
    void ServerInteract(AActor *Target);

    /** 服务器处理无需长按的普通物品拾取 */
    UFUNCTION(Server, Reliable)
    void ServerPickup(AActor *Target);

    /** 服务器检查目标并记录长按开始时间 */
    UFUNCTION(Server, Reliable)
    void ServerBeginPickupHold(AActor *Target);

    /** 服务器确认长按时间足够，再执行取件或交付 */
    UFUNCTION(Server, Reliable)
    void ServerCompletePickupHold(AActor *Target);

    /** 服务器取消指定目标的长按记录 */
    UFUNCTION(Server, Reliable)
    void ServerCancelPickupHold(AActor *Target);

    /** 判断角色是否已晕倒；优先检查同步的眩晕标记，并检查生命值，避免标记尚未同步时仍允许操作 */
    UFUNCTION(BlueprintPure, Category = "State")
    bool IsIncapacitated() const;

    // 组件查询
    UDeliveryInteractionComponent *GetInteractionComponent() const
    {
        return InteractionComponent;
    }
    UDeliveryCarryComponent *GetCarryComponent() const
    {
        return CarryComponent;
    }

    FORCEINLINE UCapsuleComponent *GetCapsuleComponent() const
    {
        return CapsuleComponent;
    }

    FORCEINLINE USkeletalMeshComponent *GetMesh() const
    {
        return Mesh;
    }

    FORCEINLINE USpringArmComponent *GetCameraBoom() const
    {
        return CameraBoom;
    }

    FORCEINLINE UCameraComponent *GetFollowCamera() const
    {
        return FollowCamera;
    }

    FORCEINLINE UDeliveryActiveRagdollComponent *GetActiveRagdoll() const
    {
        return ActiveRagdoll;
    }

    FORCEINLINE UDeliveryRagdollCombatComponent *GetRagdollCombat() const
    {
        return RagdollCombat;
    }

    FORCEINLINE UDeliveryGrabComponent *GetGrabComponent() const
    {
        return GrabComponent;
    }

    FORCEINLINE UDeliveryGrabbableComponent *GetGrabbableComponent() const
    {
        return GrabbableComponent;
    }

    FORCEINLINE UDeliveryInteractionProbeComponent *GetInteractProbe() const
    {
        return InteractProbe;
    }

    FORCEINLINE UDeliveryInventoryComponent *GetInventoryComponent() const
    {
        return InventoryComponent;
    }

    FORCEINLINE USceneComponent *GetHeldItemAnchor() const
    {
        return HeldItemAnchor;
    }

    bool IsGrabChordHeld() const;
    UDeliveryInputComponent* GetPlayerInput() const { return PlayerInput; }

    // 技能配置查询
    FORCEINLINE TSubclassOf<UGameplayEffect> GetHealthRegenEffect() const
    {
        return HealthRegenEffect;
    }

    FORCEINLINE TSubclassOf<UGameplayEffect> GetDamageEffect() const
    {
        return DamageEffect;
    }

    FORCEINLINE TSubclassOf<UGameplayAbility> GetPunchLeftAbilityClass() const
    {
        return PunchLeftAbilityClass;
    }

    FORCEINLINE TSubclassOf<UGameplayAbility> GetPunchRightAbilityClass() const
    {
        return PunchRightAbilityClass;
    }

    FORCEINLINE float GetStunRecoverHealthPercent() const
    {
        return StunRecoverHealthPercent;
    }

  private:
    /** 取得当前拾取目标，根据目标配置选择立即拾取或长按 */
    void BeginPickupInteraction();
    /** 停止本机长按计时，并按需通知服务器 */
    void CancelPickupHold(bool bNotifyServer = true);

    // 起跳记录
    /** 服务器记录的上次成功起跳时间，单位为秒 */
    float LastServerJumpTime = -1000.0f;

};
