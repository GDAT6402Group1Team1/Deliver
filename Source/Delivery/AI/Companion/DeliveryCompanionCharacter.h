#pragma once
#include "CoreMinimal.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "DeliveryCompanionCharacter.generated.h"
class UDeliveryCompanionComponent;
class ADeliveryCompanionEncounterScene;
class UCurveVector;

UENUM(BlueprintType)
enum class EDeliveryCompanionEncounterPhase : uint8
{
    Dormant, OpeningDoor, Ejecting, Falling, AwaitingHelp, Helping, Joined
};

// 阶段与本次轨迹一并复制，客户端不从主人引用到达顺序推断演出阶段
USTRUCT(BlueprintType)
struct FDeliveryCompanionEncounterState
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) EDeliveryCompanionEncounterPhase Phase=EDeliveryCompanionEncounterPhase::Dormant;
    UPROPERTY(BlueprintReadOnly) int32 Revision=0;
    UPROPERTY(BlueprintReadOnly) double PhaseStartedAt=-1;
    UPROPERTY(BlueprintReadOnly) double EncounterStartedAt=-1;
    UPROPERTY(BlueprintReadOnly) double FallAnimationStartedAt=-1;
    UPROPERTY(BlueprintReadOnly) FVector EjectionStart=FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) FVector EjectionTarget=FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) float EjectionDuration=0;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<UCurveVector> EjectionCurve;
    UPROPERTY(BlueprintReadOnly) FName FailureReason;
};

// 复用居民的交谈与身体表现，初遇交互把归属交给同伴组件
UCLASS()
class DELIVERY_API ADeliveryCompanionCharacter : public ADeliveryNpcCharacter, public IDeliveryPackageCarrierInterface, public IDeliveryHandheldCarrierInterface
{
    GENERATED_BODY()
    friend class FDeliveryCompanionRulesTest;
    friend class FDeliveryCompanionEncounterOrderTest;
    friend class FDeliveryCompanionErrandTest;
    friend class FDeliveryCompanionAssistTest;
    friend class FDeliveryCompanionMemoryTest;
    friend class FDeliveryPackageTransferTest;
public:
    ADeliveryCompanionCharacter();
    // 用本机播放调整原音色，1 为原调
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|同伴|语音",meta=(ClampMin="0.95",ClampMax="1.08"))
    float DialoguePitch=1.045f;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<UDeliveryCompanionComponent> Companion;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<UDeliveryInteractableComponent> HelpInteractable;
    // 挂点由角色蓝图调整，物品继续使用原包裹模型与复制
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<USceneComponent> PackageAnchor;
    // 普通道具挂实际手骨，锚点始终零位移和 1 倍世界缩放，物品蓝图保存握持偏移
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<USceneComponent> HandheldAnchor;
    virtual ADeliveryHandheldItem* GetCarriedHandheldItem() const override;
    virtual APlayerState* GetHandheldOwnerPlayerState() const override;
    virtual bool CanHandleHandheldItem() const override;
    virtual bool OwnsHandheldItem(const ADeliveryHandheldItem* Item) const override;
    virtual bool CanReceiveHandheldItem(ADeliveryHandheldItem* Item) const override;
    virtual bool TryReceiveHandheldItem(ADeliveryHandheldItem* Item) override;
    virtual bool ReleaseHandheldItem(ADeliveryHandheldItem* Item) override;
    virtual void ApplyHandheldItemPresentation(ADeliveryHandheldItem* Item) override;
    virtual ADeliveryPackageItem* GetCarriedTaskPackage() const override;
    virtual APlayerState* GetTaskPlayerState() const override;
    virtual bool CanHandleTaskPackage() const override;
    virtual bool CanReceiveTaskPackage(ADeliveryPackageItem* Package) const override;
    virtual bool TryReceiveTaskPackage(ADeliveryPackageItem* Package) override;
    virtual bool ReleaseTaskPackage(ADeliveryPackageItem* Package) override;
    virtual void ApplyTaskPackagePresentation(ADeliveryPackageItem* Package) override;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|初遇") FName EncounterTaskId=TEXT("Task_001");
    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,ReplicatedUsing=OnRep_Encounter,Category="Delivery|初遇") TObjectPtr<ADeliveryCompanionEncounterScene> EncounterScene;
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") bool HasEncounterStarted() const { return Encounter.Phase!=EDeliveryCompanionEncounterPhase::Dormant; }
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") FDeliveryCompanionEncounterState GetEncounterState() const { return Encounter; }
    bool IsEncounterAirborne() const;
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") bool IsWaitingForHelp() const;
    bool HasFinishedEncounterFall() const;
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") bool IsBeingHelpedUp() const;
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") double GetEncounterStartTime() const { return Encounter.EncounterStartedAt; }
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") double GetEncounterElapsed() const;
    double GetHelpStartTime() const { return IsBeingHelpedUp()?Encounter.PhaseStartedAt:-1; }
    double GetEncounterFallStartTime() const { return Encounter.FallAnimationStartedAt; }
    void BeginHelpUp();
    // 撤销 Joined 后的辅助起身和旧长按，不改变主人或持续安排
    void CancelAssistedHelp();
    void ResetForNewOwner();
    virtual bool IsKnockedDown() const override;
    virtual void ReceiveImpactFrom(const FVector& Impulse,const FVector& Point,AActor* Source) override;
    virtual void ReceiveMeleeHitFrom(const FVector& Impulse,const FVector& Point,AActor* Source,FGuid PunchId) override;
    bool WasDownBeforeMeleeHit(FGuid PunchId) const { return PunchId==LastMeleePunchId && bWasDownBeforeMelee; }
    // 跟随组件负责同伴站位，避免两个导航请求互相替换
    virtual bool CanAdjustStandingPosition() const override { return false; }
    virtual bool CanExecute(EDeliveryNpcAction Action,AActor* Target) const override;
    virtual bool CanExecuteForRequester(EDeliveryNpcAction Action,AActor* Target,APlayerState* Requester) const override;
    virtual bool CanExecuteGameplayRule(EDeliveryNpcAction Action,AActor* Target,int32 RuleRevision) const override;
    virtual void ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const override;
    virtual bool ExecuteAction(const FDeliveryNpcPublicState& State) override;
    virtual void StopAction(FGuid ActionId) override;
    // 蓝图配置老板、编织袋等初遇表现，服务器时间供迟到客户端接上进度
    UFUNCTION(BlueprintImplementableEvent,Category="Delivery|初遇") void OnEncounterStarted();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Landed(const FHitResult& Hit) override;
    virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode,uint8 PreviousCustomMode=0) override;
private:
    bool CanHelp(APawn* Helper,bool bCheckAvailability);
    bool Help(APawn* Helper);
    bool CanContinueAssistedHelp(APawn* Helper,FGuid DownEpisode) const;
    void OnCombatDownChanged(FGuid DownEpisode);
    void OnCombatRecoveryFinished(FGuid DownEpisode,APawn* Helper);
    void UpdateHelpInteraction();
    // 这个短动作只拥有对白与注视，结束时不能清掉同伴导航
    FGuid PassiveActionId;
    FGuid LastMeleePunchId;
    bool bWasDownBeforeMelee=false;
    UFUNCTION() void OnRep_Encounter();
    UPROPERTY(ReplicatedUsing=OnRep_Encounter) FDeliveryCompanionEncounterState Encounter;
    double GetEncounterServerTime() const;
    void SetEncounterPhase(EDeliveryCompanionEncounterPhase Phase);
    void UpdateEncounter();
    void UpdateEncounterPresentation();
    bool HasEncounterOrderStarted() const;
    bool CanWitnessEncounter(const APawn* Pawn) const;
    bool TryStartEncounter();
    bool TryStartEjection();
    bool HasClearEjectionPath(const FDeliveryCompanionEncounterState& State) const;
    void ApplyEjectionMotion();
    void RemoveEjectionMotion();
    void BeginEncounterFall(FName Reason);
    void ResetUnstartedEncounter(FName Reason);
    uint16 EjectionMotionId=0;
    int32 AppliedMotionRevision=INDEX_NONE;
    int32 FinishedMotionRevision=INDEX_NONE;
    bool bClientMotionWasDisabled=false;
    bool bSavedOrientToMovement=false;
    bool bSavedControllerRotation=false;
    bool bEjectionRotationSaved=false;
    bool bReportedEncounterConfiguration=false;
    bool bReportedLandingTimeout=false;
    bool bAnnouncedJoining=false;
    double NextEncounterCheck=0;
};
