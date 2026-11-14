#pragma once
#include "CoreMinimal.h"
#include "AITypes.h"
#include "Components/ActorComponent.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "AI/Companion/DeliveryCompanionHomeTypes.h"
#include "World/DeliveryHomeUseExecutor.h"
#include "Grab/DeliveryObjectCarryProvider.h"
#include "Carry/DeliveryHandheldUseExecutor.h"
#include "GameplayAbilitySpec.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryCompanionComponent.generated.h"

class APlayerState;
class ADeliveryCompanionCharacter;
class UDeliveryCompanionTuning;
class UDeliveryLocationComponent;
class UDeliveryTaskDefinition;
class UDeliveryTaskManagerComponent;
class APlayerController;
class ADeliveryPlayerController;
class ADeliveryPackageItem;
class ADeliveryHandheldItem;
class UAbilitySystemComponent;
class ADeliveryCompanionMotorbike;
class ADeliveryMotorbike;
class UDeliveryGrabbableComponent;
class UGA_UseHomeObject;
struct FGameplayEventData;
struct FGameplayTag;

UENUM(BlueprintType)
enum class EDeliveryCompanionMode : uint8 { Unrecruited, Follow, Stay, GoHome };

UENUM(BlueprintType)
enum class EDeliveryCompanionErrandPhase : uint8 { Idle, GoToPickup, ReceiveFromOwner, GoToDelivery, ReturnToOwner, WaitingForOwner, WaitingForCapacity };

// 同一时刻只有一种安排可以拥有走向车辆和驾驶请求
enum class EDeliveryCompanionTravelPurpose : uint8 { None, Errand, Follow, ReturnToStay, GoHome };

// 委托与原来的跟随/等候模式分开，短暂倾听和受击不会把已接安排清掉
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryCompanionErrand
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcAction Action=EDeliveryNpcAction::None;
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcActionStatus Status=EDeliveryNpcActionStatus::Idle;
    UPROPERTY(BlueprintReadOnly) EDeliveryCompanionErrandPhase Phase=EDeliveryCompanionErrandPhase::Idle;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<UDeliveryTaskDefinition> Task;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<ADeliveryPackageItem> Package;
    // 普通物品的安排目标，不代表实际持有，实际持物仍只有 CarriedPackage 一个槽
    UPROPERTY(BlueprintReadOnly) TWeakObjectPtr<ADeliveryHandheldItem> Item;
    UPROPERTY(BlueprintReadOnly) int32 CommandRevision=0;
    UPROPERTY(BlueprintReadOnly) FName Reason;
};

// 服务器保存主人和持续安排，行为树是导航的唯一调用者
// 对话只改变模式，倾听、受击与乘车暂停不会清掉主人的安排
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryCompanionComponent : public UActorComponent, public IDeliveryHomeUseExecutor, public IDeliveryObjectCarryProvider, public IDeliveryHandheldUseExecutor
{
    GENERATED_BODY()
    friend class FDeliveryCompanionRulesTest;
    friend class FDeliveryPackageTransferTest;
    friend class FDeliveryCompanionErrandTest;
    friend class FDeliveryCompanionAssistTest;
    friend class FDeliveryCompanionMemoryTest;
public:
    UDeliveryCompanionComponent();
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<UDeliveryCompanionTuning> Tuning;
    UPROPERTY(Replicated,EditInstanceOnly,BlueprintReadOnly,Category="Delivery|同伴|车辆") TObjectPtr<ADeliveryCompanionMotorbike> ErrandVehicle;
    bool IsUsingErrandVehicle() const;
    ADeliveryCompanionMotorbike* GetFollowMotorbike() const;
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|车辆") EDeliveryCompanionTravelMode GetTravelMode() const { return TravelMode; }
    void ExtendTravelDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    bool EnsureOnFoot();
    // ASC 完成初始化后由角色调用，只授予本组件负责的家居能力
    void InitializeHomeAbility();
    const FDeliveryCompanionHomeState& GetHomeState() const { return HomeState; }
    bool HasActiveHomeArrangement() const;
    bool OwnsHomeSource(const UDeliveryHomeObjectComponent* Source) const;
    bool ApplyHomeChoice(const FDeliveryCompanionHomeChoice& Choice,APlayerState* Requester,int32 ExpectedRevision);
    bool CanApplyHomeChoice(const FDeliveryCompanionHomeChoice& Choice,APlayerState* Requester,int32 ExpectedRevision) const;
    void ExtendHomeDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    // BT 持续分支调用，普通中断仅暂停本组件自己的路径
    void UpdateHomeArrangement();
    void StopHomeMovement();
    void CancelHomeArrangement(FName Reason,bool bForce=false);
    virtual bool StartHomeUse(const UDeliveryHomeUseRequest& Request) override;
    virtual void CancelHomeUse(FGuid ArrangementId,FName Reason) override;
    virtual FDeliveryHomeUseFinished& GetHomeUseFinishedDelegate() override { return HomeUseFinished; }
    virtual bool GetObjectCarryPose(const AActor* Object,const FGuid& CarryId,FVector& OutLocation,FVector& OutForward) const override;
    virtual void OnObjectCarryReleased(AActor* Object,const FGuid& CarryId,EDeliveryObjectCarryReleaseReason Reason) override;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|能力") TSubclassOf<UGA_UseHomeObject> HomeUseAbilityClass;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|搬运") TObjectPtr<UAnimSequence> PropCarryAnimation;
    // 留空使用角色胶囊的实时位置，指定骨骼时偏移按该骨骼坐标且不继承网格缩放
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|搬运") FName PropCarrySocket;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|搬运",meta=(Units="cm")) FVector PropCarryOffset=FVector(70,0,0);
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|搬运",meta=(ClampMin="0.1",ClampMax="5",Units="s")) float PropCarryBlendSeconds=.6f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|搬运",meta=(ClampMin="1",ClampMax="15",Units="cm")) float PropCarryReachTolerance=4;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|搬运",meta=(ClampMin="20",ClampMax="100",Units="cm")) float PropCarryPauseDistance=60;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|摆放",meta=(ClampMin="0.1",ClampMax="2",Units="s")) float PropPlacementSettleSeconds=.4f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|摆放",meta=(ClampMin="1",ClampMax="15",Units="s")) float PropPlacementSettleTimeout=4;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|摆放",meta=(ClampMin="1",ClampMax="20",Units="cm/s")) float PropPlacementMaxSpeed=5;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|摆放",meta=(ClampMin="1",ClampMax="20",Units="deg/s")) float PropPlacementMaxAngularSpeed=5;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|接近",meta=(ClampMin="1",ClampMax="20",Units="cm")) float HomeUseReachTolerance=6;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|接近",meta=(ClampMin="1",ClampMax="15",Units="deg")) float HomeUseYawTolerance=5;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|时限",meta=(ClampMin="5",ClampMax="300",Units="s")) float HomeArrangementTimeout=90;
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") APlayerState* GetOwnerPlayerState() const { return OwnerPlayerState; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") EDeliveryCompanionMode GetMode() const { return Mode; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") int32 GetCommandRevision() const { return CommandRevision; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") FName GetSuspensionReason() const { return SuspensionReason; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") int32 GetAffinity() const { return Affinity; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|战斗") AActor* GetAssistTarget() const { return AssistTarget; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|战斗") int32 GetAssistRevision() const { return AssistRevision; }
    // 只检查当前真实命中建立的护主目标，模型和访客不能写入这个版本
    bool CanExecuteAssist(AActor* Target,int32 RuleRevision) const;
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|关系") FString GetRelationshipStage() const;
    // 服务器当前归属内的真实累计次数，解除主人时清空；战斗扶起不包含首次结识
    int32 GetAssistedRecoveryCount() const { return AssistedRecoveryCount; }
    int32 GetDeliveryTogetherCount() const { return DeliveryTogetherCount; }
    int32 GetOwnerPunchCount() const { return OwnerPunchCount; }
    int32 GetOwnerDownPunchCount() const { return OwnerDownPunchCount; }
    int32 GetOwnerVehicleHitCount() const { return OwnerVehicleHitCount; }
    bool RecordAssistedRecovery(FGuid DownEpisode,APawn* Helper);
    bool CanOfferCaterpillar(APlayerState* Requester) const;
    bool CanRequestCaterpillarOffer(APlayerState* Requester) const;
    bool HasCaterpillarOffer() const { return bCaterpillarOffered && !bCaterpillarGiven; }
    bool CanAcceptCaterpillar(APawn* Interactor) const;
    // 只由主人实际交互调用，true 表示原背包接口已收到真实道具
    bool AcceptCaterpillar(APawn* Interactor);
    bool CanGoHome(APlayerState* Requester) const;
    void RecordOwnerVehicleHit(AActor* Source);
    bool CanRecruit(APawn* Helper) const;
    // 仅由服务器长按交互完成入口调用，true 表示归属已经写入
    bool TryRecruit(APawn* Helper);
    bool CanCommand(APlayerState* Requester) const;
    // 编号必须来自本次模型请求，true 只表示新模式已生效
    bool ApplyCommand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
        UDeliveryTaskDefinition* ExpectedTask=nullptr,ADeliveryPackageItem* ExpectedPackage=nullptr,
        EDeliveryCompanionTravelMode RequestedTravelMode=EDeliveryCompanionTravelMode::Unspecified);
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|跑腿") bool HasActiveErrand() const;
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|跑腿") FDeliveryCompanionErrand GetErrand() const { return Errand; }
    bool CanArrangeErrand(EDeliveryNpcAction Action,APlayerState* Requester) const;
    // 仅行为树调用，执行移动和已验证的物品接口；暂停只停本任务导航
    void UpdateErrand();
    void StopErrandMovement();
    void RefreshItemObservations();
    void ExtendItemDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    // 普通道具的短动作只跟踪当前实见物或同槽实际自持物，不把旧记录当当前位置
    bool CanUseObservedItemTarget(const ADeliveryHandheldItem* Item) const;
    bool CanArrangeItem(EDeliveryNpcAction Action,ADeliveryHandheldItem* Item,APlayerState* Requester) const;
    bool CanApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision) const;
    bool ApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision,
        EDeliveryCompanionTravelMode RequestedTravelMode=EDeliveryCompanionTravelMode::Unspecified);
    virtual bool ExecuteHandheldUse(const UDeliveryHandheldUseRequest& Request,FName& Reason) override;
    virtual void FinishHandheldUse(FGuid UseId,bool bSucceeded,FName Reason) override;
    ADeliveryHandheldItem* GetCarriedItem() const;
    bool CanReceiveCarriedItem(ADeliveryHandheldItem* Item) const;
    bool TryReceiveCarriedItem(ADeliveryHandheldItem* Item);
    bool ReleaseCarriedItem(ADeliveryHandheldItem* Item);
    ADeliveryPackageItem* GetCarriedTaskPackage() const;
    bool CanReceiveTaskPackage(ADeliveryPackageItem* Package) const;
    bool TryReceiveTaskPackage(ADeliveryPackageItem* Package);
    bool ReleaseTaskPackage(ADeliveryPackageItem* Package);
    void ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    // 只允许 BT 的持续分支调用，其他分支执行时先 StopFollowMovement
    void UpdateFollow();
    void StopFollowMovement();
    // 由发送查询的拥有者PC转交结果，只记录可见性，不在RPC中移动角色
    void AcceptCatchUpVisibility(APlayerController* Player,FGuid RequestId,APawn* ViewPawn,AActor* ViewTarget,bool bUnseen);
    const UDeliveryCompanionTuning& GetTuning() const;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* TickFunction) override;
private:
    ADeliveryCompanionCharacter* GetCompanion() const;
    struct FObservedPublicPlace
    {
        TWeakObjectPtr<UDeliveryLocationComponent> Location;
        FDeliveryNpcObservedPlace View;
    };
    // 当次读取本世界公开地点，不缓存可见性或创建新行动证据
    void CollectObservedPlaces(TArray<FObservedPublicPlace>& Out) const;
    void ExtendWorldDecisionRequest(FDeliveryNpcDecisionRequest& Request) const;
    // 沿既有服务器生命周期更新旧见闻，读取请求时只复制并计算观察年龄
    void RefreshPublicPlaceMemories();
    void RecordPublicPlaceMemory(const FObservedPublicPlace& Place,double ObservedAt);
    void PrunePublicPlaceMemoryIndex();
    void CopyCompanionMemories(TArray<FDeliveryNpcCompanionMemory>& Out) const;
    APawn* GetOwnerPawn() const;
    bool FindFollowDestination(APawn* Pawn,FVector& Out) const;
    bool FindWaitDestination(APawn* Pawn,FVector& Out) const;
    bool IsDestinationClear(const FVector& Destination) const;
    bool IsOutsideTaskEntrance(const FVector& Destination) const;
    bool TryBeginCatchUp(APawn* Pawn,const FVector& Destination);
    bool UpdateCatchUp(APawn* Pawn);
    bool CanUseCatchUpDestination(APawn* Pawn,const FVector& Destination) const;
    bool AreCatchUpViewersCurrent() const;
    void SendCatchUpQueries();
    void CancelCatchUp();
    void SetSuspension(FName Reason);
    void ReleaseOwner();
    void RefreshOwnerCombatSubscription();
    void UnbindOwnerCombat();
    void OnOwnerConfirmedHit(FGameplayTag EventTag,const FGameplayEventData* EventData);
    bool IsAllowedAssistTarget(const AActor* Target) const;
    void UpdateAssist();
    void CancelAssist(FName Reason);
    UFUNCTION() void OnAssistTargetDestroyed(AActor* DestroyedActor);
    // true 表示已经可以执行末段步行，false 表示仍在取车、骑行或等待安全下车
    bool UpdateVehicleTravel(const FVector& Goal,float ReachRadius,EDeliveryCompanionTravelPurpose Purpose);
    void StopVehicleTravel(EDeliveryCompanionTravelPurpose Purpose);
    void StopVehicleApproach();
    void StopWalkingForVehicle();
    void ReportVehiclePause(FName Reason);
    void ReleaseErrandVehicle();
    void ApplyTravelInstruction(EDeliveryCompanionTravelMode NewMode);
    bool CanTakeNearbyPackage() const;
    UPROPERTY(Replicated) EDeliveryCompanionTravelMode TravelMode=EDeliveryCompanionTravelMode::Unspecified;
    // 只有主人接受为跟随指令的骑车安排才能启动跟车，跑腿的出行选择独立
    bool bRideWhileFollowing=false;
    bool RecordMemory(FGuid EventId,FName Kind,const FString& Description,int32 AffinityDelta);
    void RecordRecruitMemory();
    bool RecordDeliveryMemory(UDeliveryTaskDefinition* Task,APlayerState* Deliverer);
    void RecordOwnerPunch(FGuid PunchId,bool bWasDown);
    void ClearCompanionMemories();
    void UpdateGoHome();
    bool BeginHomeChoice(const FDeliveryCompanionHomeChoice& Choice,FGuid ArrangementId,FName& OutReason);
    bool MoveForHome(const FVector& Goal,float Radius);
    bool AlignForHome(const FTransform& Use);
    void SetHomePhase(EDeliveryCompanionHomePhase Phase);
    void SetHomePose(UAnimSequence* Animation,bool bLoop,bool bUpperBody);
    void RefreshHomeCarryPose();
    void TickHomeLifecycle();
    void ShutdownHomeExecution();
    void FinishHomeArrangement(EDeliveryCompanionHomePhase Phase,FName Reason,bool bKeepProp=false);
    void BeginLeavingHomeUse(FName Reason);
    void ReleaseHomeClaim(FDeliveryHomeUseClaim& Claim,FName Reason);
    void ReleaseHomeProp(FName Reason);
    bool BeginHomePropCarry(FName& OutReason);
    bool GetHomeHandPose(FVector& OutLocation,FVector& OutForward) const;
    bool IsHomePropAtHand() const;
    bool ValidateHomePropTransfer(FName& OutReason) const;
    void ClearHomePropTransfer();
    void BeginHomePropSegment(bool bToTransferPoint);
    bool HasReachedHomePropTransferPoint() const;
    bool IsHomePropLagging() const;
    bool IsCurrentHome(FGuid ArrangementId) const;
    bool NeedsHomeExit() const;
    void StartPendingHomeChoice();
    void OnHomeObjectUseEnded(FGuid ActionId,FName Reason);
    double GetHomeServerTime() const;
    UFUNCTION() void OnRep_HomeState();
    void UpdateHomeSeatPresentation();
    bool BeginHomeSeatMove(const FTransform& FeetGoal,UAnimSequence* Animation,FName& OutReason);
    bool UpdateHomeSeatMove(const FTransform& FeetGoal,FName& OutReason);
    void ReleaseHomeSeatMove(bool bClearState);
    FDeliveryCompanionSeatRuntime HomeSeatRuntime;
    UPROPERTY(ReplicatedUsing=OnRep_HomeState) FDeliveryCompanionHomeState HomeState;
    FDeliveryCompanionHomeChoice ActiveHomeChoice;
    FDeliveryCompanionHomeChoice PendingHomeChoice;
    FGuid PendingHomeArrangementId;
    int32 PendingHomeCommandRevision=0;
    FDeliveryHomeUseClaim HomeUseClaim;
    FDeliveryHomeUseClaim HomeTakeClaim;
    FGuid HomePropCarryId;
    TWeakObjectPtr<UDeliveryGrabbableComponent> HomeGrabbable;
    TWeakObjectPtr<APlayerState> HomeRequester;
    FGameplayAbilitySpecHandle HomeAbilityHandle;
    TWeakObjectPtr<UAbilitySystemComponent> HomeAbilitySystem;
    FDeliveryHomeUseFinished HomeUseFinished;
    FAIRequestID HomeMoveId=FAIRequestID::InvalidRequest;
    FVector HomeMoveGoal=FVector::ZeroVector;
    FVector HomeFocusPoint=FVector::ZeroVector;
    bool bHomeOwnsNavigation=false;
    bool bHomeOwnsFocus=false;
    double NextHomeNavigationAt=0;
    double LastHomeUpdateAt=0;
    double HomePhaseStartedAt=0;
    float HomeActiveSeconds=0;
    FName HomeLeaveReason;
    FName LastHomePause;
    bool bHomeUseStarted=false;
    bool bHomeExitAnimationFinished=false;
    bool bChangingHomeArrangement=false;
    bool bCommittingHomeUse=false;
    bool bHomeEndingPlay=false;
    FVector PropBlendFrom=FVector::ZeroVector;
    FVector PropBlendForward=FVector::ForwardVector;
    // 中转点在当前家具坐标上冻结，首段保持实物开始时的朝向
    FDeliveryHomePropTransferPoint HomePropTransferPoint;
    bool bMovingToPropTransferPoint=false;
    FTransform PropPlacementTarget;
    double PropBlendStartedAt=0;
    bool bPlacingHomeProp=false;
    TWeakObjectPtr<AActor> ReleasedPlacementProp;
    double PlacementReleasedAt=0;
    double PlacementStableSince=0;
    void UpdateCaterpillarOpportunity();
    bool HasCaterpillarOpportunity() const;
    bool StoreCarriedItem(ADeliveryHandheldItem* Item);
    void DropCarriedItem();
    bool CanSeeWorldItem(const ADeliveryHandheldItem* Item,FVector& ObservedPoint) const;
    void UpdateItemErrand();
    bool RunItemTransfer(bool bReturnToOwner,FName& Reason);
    bool IsCurrentItemUse(FGuid UseId,int32 Revision,ADeliveryHandheldItem* Item) const;
    void CancelItemUse();
    void ShutdownItemExecution();
    struct FSeenItem
    {
        TWeakObjectPtr<ADeliveryHandheldItem> Item;
        FVector Location=FVector::ZeroVector;
        double SeenAt=0;
        double FactAt=0;
        FGuid EventId;
        bool bVisible=false;
    };
    TArray<FSeenItem> SeenItems;
    FGuid HeldItemEventId;
    TWeakObjectPtr<ADeliveryHandheldItem> ObservedHeldItem;
    double HeldItemObservedAt=0;
    double NextItemObservationAt=0;
    FVector ItemLastSeenLocation=FVector::ZeroVector;
    double ItemLastSeenAt=0;
    FGameplayAbilitySpecHandle ItemAbilityHandle;
    TWeakObjectPtr<UAbilitySystemComponent> ItemAbilitySystem;
    FGuid ItemUseId;
    bool bItemUseResolved=false;
    bool bItemUseSucceeded=false;
    FName ItemUseReason;
    double NextItemOwnerRetryAt=0;
    UDeliveryTaskDefinition* GetCommandTask() const;
    ADeliveryPackageItem* GetCommandPackage() const;
    APawn* GetOwnerPackageCarrier() const;
    bool StartErrand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
        UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,
        EDeliveryCompanionTravelMode RequestedTravelMode=EDeliveryCompanionTravelMode::Unspecified);
    void FinishErrand(EDeliveryNpcActionStatus Status,FName Reason);
    void SetErrandPhase(EDeliveryCompanionErrandPhase Phase);
    void ReportErrandPause(FName Reason);
    bool MoveForErrand(const FVector& Goal,float ReachRadius,AActor* InteractionTarget);
    AActor* ResolveErrandLocation(bool bPickup) const;
    UFUNCTION() void OnRep_CarriedPackage();
    UFUNCTION() void OnCarriedPackageDestroyed(AActor* DestroyedActor);
    UFUNCTION() void OnTaskCompleted(UDeliveryTaskDefinition* Task,const FDeliveryRewardBreakdown& Reward,APlayerState* Deliverer);
    UPROPERTY(Replicated) TObjectPtr<APlayerState> OwnerPlayerState;
    UPROPERTY(Replicated) EDeliveryCompanionMode Mode=EDeliveryCompanionMode::Unrecruited;
    UPROPERTY(Replicated) int32 CommandRevision=0;
    UPROPERTY(Replicated) FName SuspensionReason;
    UPROPERTY(Replicated) int32 Affinity=0;
    UPROPERTY(Replicated) TObjectPtr<AActor> AssistTarget;
    UPROPERTY(Replicated) int32 AssistRevision=0;
    FGuid AssistEvidenceId;
    double AssistExpiresAt=0;
    // 当前连续交手各方向最多记一次，切目标、过期或取消后清空；仅服务器使用
    uint8 RememberedAssistHitDirections=0;
    FVector AssistOrigin=FVector::ZeroVector;
    TArray<FGuid> RecentAssistHits;
    TWeakObjectPtr<UAbilitySystemComponent> BoundOwnerCombat;
    FDelegateHandle OwnerCombatHandle;
    TArray<FDeliveryNpcCompanionMemory> Memories;
    // 只索引 Memories 中最多六条地点见闻，不保活地点，也不保存导航坐标
    struct FRememberedPublicPlace
    {
        TWeakObjectPtr<UDeliveryLocationComponent> Location;
        FGuid EventId;
        double ObservedAt=0;
    };
    TArray<FRememberedPublicPlace> RememberedPublicPlaces;
    double NextPublicPlaceObservationAt=0;
    // 当局去重不随模型最近 24 条记忆淘汰，旧事件重发不能再次加减关系
    TSet<FGuid> RecordedMemoryEvents;
    bool bReportedMemoryLimit=false;
    TSet<TWeakObjectPtr<UDeliveryTaskDefinition>> RememberedDeliveries;
    TSet<FGuid> RememberedVehicleKnockdowns;
    int32 AssistedRecoveryCount=0;
    int32 DeliveryTogetherCount=0;
    int32 OwnerPunchCount=0;
    int32 OwnerDownPunchCount=0;
    int32 OwnerVehicleHitCount=0;
    UPROPERTY(Replicated) bool bCaterpillarOffered=false;
    UPROPERTY(Replicated) bool bCaterpillarGiven=false;
    bool bGivingCaterpillar=false;
    bool bAnnouncedCaterpillarOpportunity=false;
    FGuid CaterpillarOpportunityId;
    double CaterpillarOpportunityExpiresAt=0;
    double NextCaterpillarOpportunityAt=0;
    FVector VehicleLegGoal=FVector::ZeroVector;
    EDeliveryCompanionTravelPurpose VehicleTravelPurpose=EDeliveryCompanionTravelPurpose::None;
    bool bVehicleLegComplete=false;
    bool bVehicleNeedsDismount=false;
    int32 VehicleLegRevision=-1;
    EDeliveryCompanionErrandPhase VehicleLegPhase=EDeliveryCompanionErrandPhase::Idle;
    FAIRequestID VehicleApproachMoveId=FAIRequestID::InvalidRequest;
    bool bVehicleOwnsApproach=false;
    bool bVehicleHasDriveGoal=false;
    FVector VehicleParkingGoal=FVector::ZeroVector;
    double NextVehicleNavigationAt=0;
    // 保留旧反射名，实际为唯一单手持物槽；任务查询仅返回其中的 Package 子类
    // 只有服务器交接接口改写，取消安排不会删除实际携带物
    UPROPERTY(ReplicatedUsing=OnRep_CarriedPackage) TObjectPtr<ADeliveryHandheldItem> CarriedPackage;
    UPROPERTY(Replicated) FDeliveryCompanionErrand Errand;
    FAIRequestID ErrandMoveId=FAIRequestID::InvalidRequest;
    FVector ErrandDestination=FVector::ZeroVector;
    FVector ErrandGoal=FVector::ZeroVector;
    double NextErrandNavigationAt=0;
    double LastErrandUpdateAt=0;
    float ErrandActiveSeconds=0;
    // 委托基础余量加各阶段第一次完整路径的行走时间，不随每次重寻路增长
    double ErrandTimeLimitSeconds=0;
    bool bErrandTravelBudgetAdded=false;
    // 取回远处真实停车车辆的步行时间与送取路线分开，每阶段只计入一次
    bool bErrandVehicleApproachBudgetAdded=false;
    float ErrandInteractionSeconds=0;
    FName LastErrandPause;
    // 只取消本次从主人车辆接货的交接，不影响后来开始的装卸
    TWeakObjectPtr<ADeliveryMotorbike> ErrandCargoVehicle;
    FGuid ErrandCargoHandoffId;
    void CancelPackageHandoff();
    bool bErrandOwnsNavigation=false;
    bool bHasErrandDestination=false;
    TWeakObjectPtr<UDeliveryTaskManagerComponent> BoundTaskManager;
    FVector LastDestination=FVector::ZeroVector;
    FVector LastOwnerPosition=FVector::ZeroVector;
    FVector WaitLocation=FVector::ZeroVector;
    double NextNavigationAt=0;
    bool bOwnsNavigation=false;
    FAIRequestID FollowMoveId=FAIRequestID::InvalidRequest;
    bool bHasFollowDestination=false;
    bool bReportedBlocked=false;
    // 两次按需查询属于同一模式版本，任何控制对象变化、超时或拒绝都取消补位
    struct FCatchUpViewer
    {
        TWeakObjectPtr<ADeliveryPlayerController> Player;
        TWeakObjectPtr<APawn> Pawn;
        TWeakObjectPtr<AActor> ViewTarget;
        uint64 ViewRevision=0;
        bool bReplied=false;
    };
    TArray<FCatchUpViewer> CatchUpViewers;
    FGuid CatchUpRequestId;
    TWeakObjectPtr<APawn> CatchUpOwnerPawn;
    FVector CatchUpStart=FVector::ZeroVector;
    FVector CatchUpOwnerPosition=FVector::ZeroVector;
    FVector CatchUpDestination=FVector::ZeroVector;
    FVector CatchUpStartCenter=FVector::ZeroVector;
    FVector CatchUpEndCenter=FVector::ZeroVector;
    FVector CatchUpExtent=FVector::ZeroVector;
    int32 CatchUpCommandRevision=0;
    int32 CatchUpRound=0;
    double CatchUpDeadline=0;
    double NextCatchUpRoundAt=0;
    double NextCatchUpAt=0;
    bool bCatchUpDenied=false;
};
