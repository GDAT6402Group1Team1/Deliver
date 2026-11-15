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
class ADeliveryHome;

UENUM(BlueprintType)
enum class EDeliveryGrandpaAction : uint8
{
    Idle, Reply, Startled, Receive, Thanks, FinishReply, Finished, StrollOut, StrollHome, LookAround, MoveOut
};

UENUM(BlueprintType)
enum class EDeliveryGrandpaMoveOutState : uint8 { NotStarted, Walking, Waiting, Departed };

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
    // 《汤喝完再搬》：送达后爷爷留在门边，玩家拿着斧头长按 E 催他搬走
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grandpa|Components")
    TObjectPtr<UDeliveryInteractableComponent> UrgeInteraction;

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
    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,Category="Grandpa|Home") TObjectPtr<ADeliveryHome> SharedHome;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home",meta=(ClampMin="10",ClampMax="200",Units="cm/s"))
    float MoveOutSpeed=70.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home",meta=(ClampMin="1",ClampMax="30",Units="cm"))
    float MoveOutArrivalRadius=12.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home",meta=(ClampMin="0.2",ClampMax="5",Units="s"))
    float MoveOutRetrySeconds=1.f;
    // 搬离时按真实支撑面走，超过台阶范围就等候，不越过落差或悬空完成
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home",meta=(ClampMin="0",ClampMax="40",Units="cm"))
    float MoveOutMaxStepUp=20.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home",meta=(ClampMin="0",ClampMax="80",Units="cm"))
    float MoveOutMaxStepDown=60.f;

    // 送达后先留下，被催促并说完两句台词才沿原路线搬离；关闭后恢复送达即自动搬离
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home") bool bRequireUrgeToMoveOut=true;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home",meta=(ClampMin="1",ClampMax="8",Units="s"))
    float UrgeLineSeconds=1.8f;
    // 玩家手持斧头站到这个距离内并面向爷爷约一秒，也算催他；固定规则，和长按 E 走同一段演出
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home",meta=(ClampMin="0",ClampMax="600",Units="cm"))
    float AxeThreatDistance=260.f;
    // 被吓到后逃出屋子的速度倍率
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Grandpa|Home",meta=(ClampMin="1",ClampMax="4"))
    float ScaredSpeedScale=2.4f;

    /** 防止连续重复包被当成第二次敲门，不影响原任务计时 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grandpa|Task", meta=(ClampMin="0.2", Units="s"))
    float KnockInterval = 1.f;

    UFUNCTION(BlueprintPure, Category="Grandpa") int32 GetKnockCount() const { return KnockCount; }
    UFUNCTION(BlueprintPure, Category="Grandpa") bool IsDoorBroken() const;
    UFUNCTION(BlueprintPure, Category="Grandpa") bool HasReceivedDelivery() const { return bDelivered; }
    UFUNCTION(BlueprintPure, Category="Grandpa") EDeliveryGrandpaAction GetCurrentAction() const { return CurrentAction; }
    UFUNCTION(BlueprintPure,Category="Grandpa|Home") bool HasConfirmedHomeDelivery() const { return bHomeDeliveryConfirmed; }
    UFUNCTION(BlueprintPure,Category="Grandpa|Home") bool HasMovedOut() const { return MoveOutState==EDeliveryGrandpaMoveOutState::Departed; }
    UFUNCTION(BlueprintPure,Category="Grandpa|Home") EDeliveryGrandpaMoveOutState GetMoveOutState() const { return MoveOutState; }
    UFUNCTION(BlueprintPure,Category="Grandpa|Home") FName GetMoveOutWaitReason() const { return MoveOutWaitReason; }
    FGuid GetConfirmedDeliveryId() const { return ConfirmedDeliveryId; }
    // 已收到汤面、还坐在门边等人催；状态来自复制字段，客户端提示与服务器判断一致
    UFUNCTION(BlueprintPure,Category="Grandpa|Home") bool IsWaitingToBeUrged() const;
    bool CanUrgeMoveOut(APawn* Interactor,bool bCheckAvailability) const;
    // 只能由权威交互调用，返回 true 表示爷爷开始收拾；实际搬离和房屋解锁仍由原路线确认
    bool UrgeMoveOut(APawn* Interactor);
    APlayerState* GetUrgedBy() const;
    // 只由行为树驱动，取消后停止走动，保留已经走到的位置与路点
    bool BeginMoveOut();
    void UpdateMoveOut(float DeltaSeconds);
    void CancelMoveOut();

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
    UPROPERTY(ReplicatedUsing=OnRep_State) bool bHomeDeliveryConfirmed=false;
    UPROPERTY(ReplicatedUsing=OnRep_Action) EDeliveryGrandpaMoveOutState MoveOutState=EDeliveryGrandpaMoveOutState::NotStarted;
    UPROPERTY(ReplicatedUsing=OnRep_Action) FName MoveOutWaitReason;
    // 0 还没被催，1 和 2 是两句催促台词，3 开始动身
    UPROPERTY(ReplicatedUsing=OnRep_Action) uint8 UrgeStage=0;
    FTimerHandle UrgeTimer;
    // 只在服务器记录是谁催的，房屋解锁时交给共享房屋区分提示
    TWeakObjectPtr<APlayerState> UrgedBy;
    bool StartUrge(APawn* Interactor,const TCHAR* Source);
    APawn* FindAxeThreat() const;
    TWeakObjectPtr<APawn> AxeThreatPawn;
    double AxeThreatSince=-1;
    // 逃出屋子时被人或杂物挡住的起始时间，挡太久就直接挪到下一个路点
    double FleeBlockedSince=-1;
    void AdvanceUrge();
    FGuid ConfirmedDeliveryId;
    int32 DeparturePointIndex=0;
    bool bMoveOutActive=false;
    double NextMoveOutRetry=0;
    FVector HomeLocation, StrollLocation, MotionTarget;
    bool bMotionActive = false;
    bool bReplyPending = false;
    float LastKnockTime = -1000.f;
    FTimerHandle BindTimer;

    void BindTaskManager();
    void UpdateBlackboard();
    void RefreshInteractions();
    void HandleDeliveryConfirmed(UDeliveryTaskDefinition* Task,AActor* Package,APlayerState* Player);
    void SetMoveOutWaiting(FName Reason);
    bool FindMoveOutSupport(const FVector& Position,FVector& OutSupportedPosition) const;
    void SetMoveOutWalking();
    UFUNCTION() void OnRep_FrontDoor();
    UFUNCTION() void HandleDoorBroken();
    UFUNCTION() void HandleTaskChanged(UDeliveryTaskDefinition* Task, EDeliveryTaskStatus Status);
    UFUNCTION() void OnRep_State();
    UFUNCTION() void OnRep_Action();
};
