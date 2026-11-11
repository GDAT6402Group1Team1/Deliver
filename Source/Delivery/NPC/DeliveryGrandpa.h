#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryGrandpa.generated.h"

class ADeliveryBreakableDoor;
class UAnimSequence;
class UBehaviorTree;
class UCapsuleComponent;
class UDeliveryInteractableComponent;
class UDeliveryTargetComponent;
class UDeliveryTaskDefinition;
class UDeliveryTaskManagerComponent;
class USkeletalMeshComponent;
class USceneComponent;
class UTextRenderComponent;

UENUM(BlueprintType)
enum class EDeliveryGrandpaAction : uint8
{
    Idle, Reply, Startled, Receive, Thanks, FinishReply, Finished, StrollOut, StrollHome, LookAround
};

/** 第一单的收件人，服务器维护敲门与收件条件，行为树蓝图选择动作并复制表现 */
UCLASS()
class DELIVERY_API ADeliveryGrandpa : public APawn
{
    GENERATED_BODY()
public:
    ADeliveryGrandpa();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grandpa|Components")
    TObjectPtr<USkeletalMeshComponent> Body;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grandpa|Components")
    TObjectPtr<UDeliveryTargetComponent> DeliveryTarget;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grandpa|Components")
    TObjectPtr<UDeliveryInteractableComponent> ReceiveInteraction;

    UPROPERTY(EditInstanceOnly, ReplicatedUsing=OnRep_FrontDoor, BlueprintReadOnly, Category="Grandpa|Task")
    TObjectPtr<ADeliveryBreakableDoor> FrontDoor;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Grandpa|AI")
    TObjectPtr<UBehaviorTree> Behavior;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Grandpa|Animation")
    TObjectPtr<UAnimSequence> IdleAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Grandpa|Animation")
    TObjectPtr<UAnimSequence> TalkAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Grandpa|Animation")
    TObjectPtr<UAnimSequence> StartledAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Grandpa|Animation")
    TObjectPtr<UAnimSequence> ReceiveAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Grandpa|Animation")
    TObjectPtr<UAnimSequence> WalkAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Grandpa|Animation")
    TObjectPtr<UAnimSequence> LookAnimation;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grandpa|Movement", meta=(ClampMin="0", ClampMax="150", Units="cm"))
    float StrollDistance = 70.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grandpa|Movement", meta=(ClampMin="10", ClampMax="100", Units="cm/s"))
    float StrollSpeed = 35.f;

    /** 防止连续重复包被当成第二次敲门，不影响原任务计时 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grandpa|Task", meta=(ClampMin="0.2", Units="s"))
    float KnockInterval = 1.f;

    UFUNCTION(BlueprintPure, Category="Grandpa") int32 GetKnockCount() const { return KnockCount; }
    UFUNCTION(BlueprintPure, Category="Grandpa") bool IsDoorBroken() const;
    UFUNCTION(BlueprintPure, Category="Grandpa") bool HasReceivedDelivery() const { return bDelivered; }
    UFUNCTION(BlueprintPure, Category="Grandpa") EDeliveryGrandpaAction GetCurrentAction() const { return CurrentAction; }

    /** 只能由权威交互调用，返回本次敲门是否被接受 */
    bool Knock(APawn* Interactor);
    bool CanKnock(APawn* Interactor, bool bCheckAvailability) const;
    bool CanReceive(AActor* Package, APlayerState* Player) const;

    /** 蓝图 BTTask 的动作出口，不推进任务或替玩家完成交付 */
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Grandpa")
    void ApplyBehaviorAction(EDeliveryGrandpaAction Action);

private:
    UPROPERTY() TObjectPtr<UCapsuleComponent> Capsule;
    UPROPERTY() TObjectPtr<UTextRenderComponent> Speech;
    UPROPERTY(Transient) TObjectPtr<UDeliveryInteractableComponent> KnockInteraction;
    UPROPERTY(Transient) TObjectPtr<USceneComponent> KnockAnchor;
    UPROPERTY(Transient) TObjectPtr<UDeliveryTaskManagerComponent> TaskManager;
    UPROPERTY(ReplicatedUsing=OnRep_State) int32 KnockCount = 0;
    UPROPERTY(ReplicatedUsing=OnRep_State) bool bDelivered = false;
    UPROPERTY(ReplicatedUsing=OnRep_Action) EDeliveryGrandpaAction CurrentAction = EDeliveryGrandpaAction::Idle;
    UPROPERTY(ReplicatedUsing=OnRep_Action) bool bWalking = false;
    FVector HomeLocation, StrollLocation, MotionTarget;
    bool bMotionActive = false;
    bool bReplyPending = false;
    float LastKnockTime = -1000.f;
    FTimerHandle BindTimer;

    void BindTaskManager();
    void UpdateBlackboard();
    void RefreshInteractions();
    UFUNCTION() void OnRep_FrontDoor();
    UFUNCTION() void HandleDoorBroken();
    UFUNCTION() void HandleTaskChanged(UDeliveryTaskDefinition* Task, EDeliveryTaskStatus Status);
    UFUNCTION() void OnRep_State();
    UFUNCTION() void OnRep_Action();
};
