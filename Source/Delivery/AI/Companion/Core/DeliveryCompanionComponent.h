#pragma once
#include "CoreMinimal.h"
#include "AITypes.h"
#include "Components/ActorComponent.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "AI/Companion/Home/DeliveryCompanionHomeTypes.h"
#include "World/Home/DeliveryHomeUseExecutor.h"
#include "Grab/DeliveryObjectCarryProvider.h"
#include "Carry/DeliveryHandheldUseExecutor.h"
#include "GameplayAbilitySpec.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryCompanionComponent.generated.h"

class ADeliveryHome;

class APlayerState;
class ADeliveryCompanionCharacter;
class ADeliveryCharacter;
class ADeliveryNpcCharacter;
class UDeliveryCompanionTuning;
class UDeliveryLocationComponent;
class UDeliveryTaskDefinition;
class UDeliveryTaskManagerComponent;
class UDeliveryTargetComponent;
class APlayerController;
class ADeliveryPlayerController;
class ADeliveryPackageItem;
class ADeliveryCaterpillarItem;
class ADeliveryHandheldItem;
class UAbilitySystemComponent;
class ADeliveryCompanionMotorbike;
class ADeliveryMotorbike;
class UDeliveryGrabbableComponent;
class UGA_UseHomeObject;
class UPathFollowingComponent;
struct FGameplayEventData;
struct FGameplayTag;
struct FDeliveryNpcMessageHandle;

UENUM(BlueprintType)
enum class EDeliveryCompanionMode : uint8 { Unrecruited, Follow, Stay, GoHome };

UENUM(BlueprintType)
enum class EDeliveryCompanionErrandPhase : uint8 { Idle, GoToPickup, ReceiveFromOwner, GoToDelivery, ReturnToOwner, WaitingForOwner, WaitingForCapacity, SearchNearby };

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
    // 服务器封存本次转出方与用途目标，客户端不能用这些引用改交接对象
    TWeakObjectPtr<AActor> ItemSourceCarrier;
    TWeakObjectPtr<ADeliveryPackageItem> ItemUsePackage;
    FName PrankInventorySource;
    bool bInsertIntoHeldPackage=false;
    bool bPrankInsertionCommitted=false;
    // 就近取还在本次委托里按步行执行，保留主人的远行设置
    bool bLocalItemTransfer=false;
    // 锁定原件来源，地上包裹失效后不能退回取件点生成另一件
    bool bRecoverGroundPackage=false;
    UPROPERTY(BlueprintReadOnly) int32 CommandRevision=0;
    UPROPERTY(BlueprintReadOnly) FName Reason;
    // 只有服务器确认在地面线索旁停稳后才开始复制查看姿势
    UPROPERTY(BlueprintReadOnly) double LowItemInspectionServerTime=-1;
    // 服务器开始伸手的时间，取物成功后保留到起身结束
    UPROPERTY(BlueprintReadOnly) double GroundPickupServerTime=-1;
    // 服务器在安全落点旁开始放件，实物释放仍由原物品接口确认
    UPROPERTY(BlueprintReadOnly) double GroundPlacementServerTime=-1;
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
    virtual bool IsObjectCarryInPlacement(const AActor* Object,const FGuid& CarryId) const override;
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
    // 只用已经在地面登记的等候点计算余量
    float GetWaitAreaRemaining() const;
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") int32 GetCommandRevision() const { return CommandRevision; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") FName GetSuspensionReason() const { return SuspensionReason; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") int32 GetAffinity() const { return Affinity; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|战斗") AActor* GetAssistTarget() const { return AssistTarget; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|战斗") int32 GetAssistRevision() const { return AssistRevision; }
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|战斗") bool HasCommittedAssist() const { return bAssistCommitted; }
    // 主人真实动手或挨打后直接加入协战；关闭后改由模型从 JoinAssist 候选里选择
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|同伴|战斗") bool bJoinAssistWithoutModel=true;
    FGuid GetAssistEvidenceId() const { return AssistEvidenceId; }
    bool CanJoinAssist(APlayerState* Requester) const;
    bool CanStopAssist(APlayerState* Requester) const;
    bool JoinAssist(APlayerState* Requester,int32 ExpectedAssistRevision);
    bool StopAssist(APlayerState* Requester,int32 ExpectedAssistRevision);
    bool IsAssistEvidenceCurrent(const AActor* Target,FGuid EvidenceId) const;
    void PauseAssistForOwnerConversation(APlayerState* Requester);
    // 只检查当前真实命中建立的护主目标，模型和访客不能写入这个版本
    bool CanExecuteAssist(AActor* Target,int32 RuleRevision) const;
    float GetAssistRange() const;
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|关系") FString GetRelationshipStage() const;
    bool CanChangeSpeechPreference(APlayerState* Requester) const;
    bool ApplySpeechPreference(APlayerState* Requester,int32 ExpectedCommandRevision,EDeliveryCompanionSpeechPreference Preference);
    int32 GetQuietSecondsRemaining() const;
    // 服务器当前归属内的真实累计次数，解除主人时清空；战斗扶起不包含首次结识
    int32 GetAssistedRecoveryCount() const { return AssistedRecoveryCount; }
    int32 GetDeliveryTogetherCount() const { return DeliveryTogetherCount; }
    int32 GetOwnerPunchCount() const { return OwnerPunchCount; }
    int32 GetOwnerDownPunchCount() const { return OwnerDownPunchCount; }
    int32 GetOwnerVehicleHitCount() const { return OwnerVehicleHitCount; }
    bool RecordAssistedRecovery(FGuid DownEpisode,APawn* Helper);
    // 只保存服务器直连已送达二狗的街坊话语，原话仍是说话人的说法
    void RecordDeliveredNeighborLine(const FDeliveryNpcMessageHandle& Handle);
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
    // 编号必须来自本次模型请求，true 表示安排已接收或原委托继续执行
    bool ApplyCommand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
        UDeliveryTaskDefinition* ExpectedTask=nullptr,ADeliveryPackageItem* ExpectedPackage=nullptr,
        EDeliveryCompanionTravelMode RequestedTravelMode=EDeliveryCompanionTravelMode::Unspecified);
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|跑腿") bool HasActiveErrand() const;
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|跑腿") FDeliveryCompanionErrand GetErrand() const { return Errand; }
    bool CanArrangeErrand(EDeliveryNpcAction Action,APlayerState* Requester) const;
    // 行为树切换短动作前检查是否应保留当前委托的移动
    bool CanKeepErrandCommand(const FDeliveryNpcPublicState& State) const;
    bool CanKeepFollowCommand(APlayerState* Requester,int32 ExpectedRevision,EDeliveryCompanionTravelMode RequestedTravelMode) const;
    // 当前委托已经接到原件，包括车辆已确认而行为树尚未推进阶段的情况
    bool HasReceivedErrandPackage() const;
    // 仅行为树调用，执行移动和已验证的物品接口；暂停只停本任务导航
    void UpdateErrand();
    void StopErrandMovement(bool bKeepVehicleSeat=false);
    void RefreshItemObservations();
    void RefreshCaterpillarObservationForOwnerRequest(APlayerState* Requester);
    bool HasNearbyItemSearch() const { return HasActiveErrand() && Errand.Phase==EDeliveryCompanionErrandPhase::SearchNearby; }
    void ExtendItemDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    // 普通道具的短动作只跟踪当前实见物或同槽实际自持物，不把旧记录当当前位置
    bool CanUseObservedItemTarget(const ADeliveryHandheldItem* Item) const;
    bool CanArrangeItem(EDeliveryNpcAction Action,ADeliveryHandheldItem* Item,APlayerState* Requester,bool bSearchLead=false) const;
    bool CanApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision) const;
    ADeliveryCharacter* GetOwnerItemCarrier() const;
    bool CanTakeOwnerItem(const ADeliveryHandheldItem* Item) const;
    FName GetCurrentOwnerItemSource(const ADeliveryHandheldItem* Item) const;
    void AddOwnerInventoryCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    bool CanArrangeCaterpillar(EDeliveryNpcAction Action,ADeliveryHandheldItem* Item,APlayerState* Requester,bool bSearchLead=false) const;
    bool IsCurrentPrankPackage(const ADeliveryPackageItem* Package,APlayerState* Requester,bool bCompanionHoldsPackage=false) const;
    ADeliveryCaterpillarItem* FindCaterpillarForOwnerRequest(FVector& Point,bool& bSeen) const;
    void RecordVisibleWorldItem(ADeliveryHandheldItem* Item,const FVector& Point,double Now);
    static bool MakeRoomForItemSearchEvidence(FDeliveryNpcDecisionRequest& Request,bool bPrioritizeCurrentItem=false);
    void AddCaterpillarItemCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    bool UpdateCaterpillarInsertion();
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
    bool FindPackageGroundLocation(const ADeliveryPackageItem& Package,const APawn& Receiver,FVector& OutLocation) const;
    bool TryPlacePackageOnGround(FName& Failure);
    void ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    // 只允许 BT 的持续分支调用，其他分支执行时先 StopFollowMovement
    void UpdateFollow();
    void StopFollowMovement(bool bKeepVehicleSeat=false);
    bool CanKeepVehicleSeatWhilePaused() const;
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
    void RefreshHomeObjectMemories();
    void CopyHomeObjectMemories(const ADeliveryHome& Home,FDeliveryNpcDecisionRequest& Request) const;
    // 已到手的共用房屋里，二狗熟悉每件家具；人在屋内或门口时不用正对着也能安排
    bool KnowsSharedHomeObject(const UDeliveryHomeObjectComponent* Object) const;
    // 实物到了手前，或已经跟在搬运距离内等过一秒，就可以开始下一段搬放
    bool IsHomePropReadyAtHand(double Now);
    double HomePropHandWaitStartedAt=-1;
    double NextHomePropHandLogAt=0;
    bool bHomePropHandAccepted=false;
    // 主人面前一直找不到放得下原尺寸的位置时，改为放在自己脚边
    double HomeOwnerPlacementBlockedSince=-1;
    // 找不到跟随站位时每两秒记一次原因，查询本身不改变安排
    mutable double NextFollowBlockedLogAt=0;
    // 跟随找不到可走的站位时，直接跳到主人身边；翻门槛、下台阶这类导航接不上的落差靠它
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随") bool bLeapToOwnerWhenFollowBlocked=true;
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随",meta=(ClampMin="150",ClampMax="1200",Units="cm")) float FollowLeapMaxDistance=750.f;
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随",meta=(ClampMin="50",ClampMax="800",Units="cm")) float FollowLeapMaxDrop=450.f;
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随",meta=(ClampMin="0",ClampMax="150",Units="cm")) float FollowLeapMaxRise=90.f;
    bool TryLeapToOwner(APawn& OwnerPawn,double Now);
    // 走不到也跳不到的时间超过这个数，直接出现在主人身边；主人在哪都能把二狗叫到跟前
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随",meta=(ClampMin="2",ClampMax="60",Units="s")) float FollowTeleportAfterSeconds=8.f;
    bool TeleportToOwner(APawn& OwnerPawn);
    double FollowStuckSince=-1;
    // 家居安排途中一直没有路时，过几秒直接到目标站位，和跟随时找不到路的处理一致
    double HomePathBlockedSince=-1;
    FVector HomePathBlockedGoal=FVector::ZeroVector;
    double FollowBlockedSince=-1;
    double NextFollowLeapAt=0;
    APawn* GetOwnerPawn() const;
    bool FindFollowDestination(APawn* Pawn,FVector& Out,TFunctionRef<bool(const FVector&)> IsOnOwnerSide) const;
    bool IsAcrossOpenFollowDoorway(const APawn* Pawn,const FVector& FromFeet) const;
    bool IsAcrossClosedFollowDoorway(const APawn* Pawn,const FVector& FromFeet,FVector* OutStreetAnchor=nullptr) const;
    void ResumeIndoorFollowAfterErrand();
    bool HasErrandPackageInTransit() const;
    void StopFollowNavigation();
    void ClearFollowMoveWatch();
    void WatchFollowMove(UPathFollowingComponent& Path,FAIRequestID MoveId,APawn& FollowedPawn,const FVector& Destination,bool bWaitingAtDoor=false);
    UPrimitiveComponent* SampleFollowMoveObstruction(const UPathFollowingComponent& Path,const APawn& FollowedPawn,double Now);
    void RememberFollowObstruction(APawn& FollowedPawn,const FVector& Destination);
    bool IsSameFollowObstructionContext(const APawn& FollowedPawn) const;
    bool HasRecentFollowObstruction(const APawn& FollowedPawn) const;
    bool IsRejectedFollowStand(const APawn& FollowedPawn,const FVector& Stand) const;
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
    void UpdateFollowMovement();
    void StopOwnerHandoffFollow();
    void UpdateAssist();
    void CancelAssist(FName Reason);
    UFUNCTION() void OnAssistTargetDestroyed(AActor* DestroyedActor);
    // true 表示已经可以执行末段步行，false 表示仍在取车、骑行或等待安全下车
    bool UpdateVehicleTravel(const FVector& Goal,float ReachRadius,EDeliveryCompanionTravelPurpose Purpose,
        const FVector* RequiredWalkGoal=nullptr);
    void StopVehicleTravel(EDeliveryCompanionTravelPurpose Purpose,bool bKeepVehicleSeat=false);
    bool IsVehicleBoardingSpotUsable(const FVector& Feet) const;
    bool KeepVehicleApproach(UPathFollowingComponent& Path,double Now);
    void StopVehicleApproach();
    void StopWalkingForVehicle();
    void SampleFollowBikeRoute(ADeliveryMotorbike* Bike);
    void ReportVehiclePause(FName Reason);
    void ReleaseErrandVehicle();
    void ApplyTravelInstruction(EDeliveryCompanionTravelMode NewMode);
    bool ShouldRideForFollow(const APawn* OwnerPawn) const;
    bool CanTransferNearbyPackage(EDeliveryNpcAction Action) const;
    bool CanWalkForNearbyItem(const FDeliveryCompanionItemChoice& Choice) const;
    UPROPERTY(Replicated) EDeliveryCompanionTravelMode TravelMode=EDeliveryCompanionTravelMode::Unspecified;
    // 未指定时跟随主人当前的步行或骑乘，跑腿的出行选择独立
    EDeliveryCompanionTravelMode FollowTravelMode=EDeliveryCompanionTravelMode::Unspecified;
    bool RecordMemory(FGuid EventId,FName Kind,const FString& Description,int32 AffinityDelta);
    void RecordRecruitMemory();
    bool RecordDeliveryMemory(UDeliveryTaskDefinition* Task,APlayerState* Deliverer,bool bDeliveredByCompanion);
    void RecordOwnerPunch(FGuid PunchId,bool bWasDown);
    void ClearCompanionMemories();
    void UpdateGoHome();
    bool BeginHomeChoice(const FDeliveryCompanionHomeChoice& Choice,FGuid ArrangementId,FName& OutReason);
    bool ResolveHomeOwnerPlacement(const UDeliveryHomeObjectComponent& Source,const APawn* OwnerPawn,
        FTransform& Entrance,FTransform& Placement) const;
    bool IsHomePlacementInFrontOfOwner(const APawn* Pawn,const FVector& GroundPoint,const AActor* Prop) const;
    void UpdateHomeOwnerPlacementApproach(FGuid ArrangementId,double Now);
    void UpdateHomePropPlacement(FGuid ArrangementId,double Now,UDeliveryHomeObjectComponent* Destination=nullptr);
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
    FTransform OwnerPlacementEntrance;
    bool bHasOwnerPlacementTarget=false;
    double PropBlendStartedAt=0;
    bool bPlacingHomeProp=false;
    TWeakObjectPtr<AActor> ReleasedPlacementProp;
    double PlacementReleasedAt=0;
    double PlacementStableSince=0;
    void UpdateCaterpillarOpportunity();
    bool HasCaterpillarOpportunity() const;
    ADeliveryCaterpillarItem* FindReachableYardCaterpillar(const APawn* Pawn,float Radius) const;
    bool StoreCarriedItem(ADeliveryHandheldItem* Item);
    void DropCarriedItem();
    bool CanSeeWorldItem(const ADeliveryHandheldItem* Item,FVector& ObservedPoint) const;
    void AddNearbyItemSearchCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    bool CanArrangeOwnerAreaSearch(APlayerState* Requester) const;
    void AddOwnerAreaSearchCandidate(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    void ResetItemSearchRoute(const FVector& Origin,bool bAlreadyOnFoot);
    TArray<FString> AreaSearchSeenNames;
    bool bAreaSearchReachedStand=false;
    void SetItemSearchFocus(const FVector& Point,float Angle);
    enum class EGroundPickupProgress : uint8 { Unavailable, Preparing, AtContact };
    EGroundPickupProgress UpdateGroundPickup(ADeliveryHandheldItem* Item,bool bInReach,TFunctionRef<bool()> IsCurrent);
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
    int32 ItemSearchPointIndex=0;
    float ItemSearchPointStartedAt=0;
    float ItemSearchLookStartedAt=-1;
    bool bItemSearchOnFoot=false;
    bool bItemRecoverySearchUsed=false;
    FVector ItemSearchSide=FVector::ForwardVector;
    TOptional<FVector> ItemSearchFocus;
    double GroundPickupAlignmentStartedAt=-1;
    FGameplayAbilitySpecHandle ItemAbilityHandle;
    TWeakObjectPtr<UAbilitySystemComponent> ItemAbilitySystem;
    FGuid ItemUseId;
    bool bItemUseResolved=false;
    bool bItemUseSucceeded=false;
    FName ItemUseReason;
    double NextItemOwnerRetryAt=0;
    UDeliveryTaskDefinition* GetCommandTask() const;
    EDeliveryCompanionTaskDoorState GetObservedTaskDoorState(const UDeliveryTaskDefinition* Task) const;
    ADeliveryPackageItem* GetCommandPackage() const;
    const ADeliveryPackageItem* GetOwnerDroppedPackage() const;
    static constexpr float GroundPackageReach=150.f;
    bool TryRecoverGroundPackage(FName& Failure);
    APawn* GetOwnerPackageCarrier() const;
    bool CanKeepPackageErrand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
        UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,EDeliveryCompanionTravelMode RequestedTravelMode) const;
    bool CanKeepItemErrand(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision,
        EDeliveryCompanionTravelMode RequestedTravelMode) const;
    bool StartErrand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
        UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,
        EDeliveryCompanionTravelMode RequestedTravelMode=EDeliveryCompanionTravelMode::Unspecified);
    void FinishErrand(EDeliveryNpcActionStatus Status,FName Reason);
    void SetErrandPhase(EDeliveryCompanionErrandPhase Phase);
    void ReportErrandPause(FName Reason,bool bKeepMounted=false);
    bool MoveForErrand(const FVector& Goal,float ReachRadius,AActor* InteractionTarget,float PreferredOffset=0.f,
        UDeliveryTargetComponent* Delivery=nullptr,const ADeliveryHome* DoorWaitHome=nullptr);
    bool IsSafeDeliveryDoorWait(const ADeliveryHome& Home,const FVector& Feet,float AcceptanceMargin=0.f) const;
    void ResetDeliveryStandRejections();
    void RefreshDeliveryStandRejections(const UDeliveryTargetComponent& Delivery);
    void RejectDeliveryStand(const UDeliveryTargetComponent& Delivery,const FVector& Stand);
    // 近期受阻站位绑定实际收件组件和固定参照点
    TArray<FVector,TInlineAllocator<4>> RejectedDeliveryStands;
    TWeakObjectPtr<const UDeliveryTargetComponent> RejectedDeliveryTarget;
    FVector RejectedDeliveryReference=FVector::ZeroVector;
    AActor* ResolveErrandLocation(bool bPickup) const;
    UFUNCTION() void OnRep_CarriedPackage();
    UFUNCTION() void OnCarriedPackageDestroyed(AActor* DestroyedActor);
    void BindTaskCompletion();
    UFUNCTION() void OnTaskCompleted(UDeliveryTaskDefinition* Task,const FDeliveryRewardBreakdown& Reward,APlayerState* Deliverer);
    UPROPERTY(Replicated) TObjectPtr<APlayerState> OwnerPlayerState;
    UPROPERTY(Replicated) EDeliveryCompanionMode Mode=EDeliveryCompanionMode::Unrecruited;
    UPROPERTY(Replicated) int32 CommandRevision=0;
    UPROPERTY(Replicated) FName SuspensionReason;
    UPROPERTY(Replicated) int32 Affinity=0;
    UPROPERTY(Replicated) TObjectPtr<AActor> AssistTarget;
    UPROPERTY(Replicated) int32 AssistRevision=0;
    UPROPERTY(Replicated) bool bAssistCommitted=false;
    FGuid AssistEvidenceId;
    // 真实现场事件只在等待初次模型选择时到期
    double PendingAssistExpiresAt=0;
    // 主人叫停后，同一对象的新命中需要主人明确重新加入
    TWeakObjectPtr<AActor> StoppedAssistTarget;
    double AssistStoppedAt=-1;
    void PauseAssistRound(FName Reason);
    // 当前连续交手各方向最多记一次，切目标、过期或取消后清空；仅服务器使用
    uint8 RememberedAssistHitDirections=0;
    TArray<FGuid> RecentAssistHits;
    TWeakObjectPtr<UAbilitySystemComponent> BoundOwnerCombat;
    FDelegateHandle OwnerCombatHandle;
    TArray<FDeliveryNpcCompanionMemory> Memories;
    // 服务器世界时间的截止点，与移动安排和亲密度分别保存
    double QuietSpeechUntil=0;
    struct FRememberedNeighborLine
    {
        TWeakObjectPtr<ADeliveryNpcCharacter> Speaker;
        FGuid MessageId;
    };
    // 只索引 Memories 已保存的各街坊最近一句，不保活人物
    TArray<FRememberedNeighborLine> RememberedNeighborLines;
    // 只索引 Memories 中最多六条地点见闻，不保活地点，也不保存导航坐标
    struct FRememberedPublicPlace
    {
        TWeakObjectPtr<UDeliveryLocationComponent> Location;
        FGuid EventId;
        double ObservedAt=0;
    };
    TArray<FRememberedPublicPlace> RememberedPublicPlaces;
    double NextPublicPlaceObservationAt=0;
    struct FRememberedHomeObject
    {
        FDeliveryNpcRememberedHomeObject View;
        double ObservedAt=0;
    };
    // 只记本局亲眼见过的名称和用途描述，不保存道具引用或上次位置
    TWeakObjectPtr<ADeliveryHome> ObservedHome;
    TMap<FGuid,FRememberedHomeObject> RememberedHomeObjects;
    double NextHomeObjectObservationAt=0;
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
    FVector VehicleApproachFeet=FVector::ZeroVector;
    bool bVehicleOwnsApproach=false;
    bool bVehicleHasDriveGoal=false;
    FVector VehicleParkingGoal=FVector::ZeroVector;
    // 驾驶目标绑定本段主人 Pawn，轨迹保留主人刚驶过的车位
    TWeakObjectPtr<APawn> VehicleFollowOwnerPawn;
    int32 OwnerHandoffFollowRevision=INDEX_NONE;
    TArray<FVector> FollowRoute;
    // 主人结束一次真实骑行后保持步行，直到再次骑车或明确改出行方式
    bool bFollowOwnerWasRiding=false;
    bool bFollowWalkAfterOwnerRide=false;
    void RefreshFollowOwnerTravel(const APawn* Pawn);
    // 跟随的步行出口检查失败后，短期避开同一个停车位置
    TArray<FVector,TInlineAllocator<4>> RejectedFollowParking;
    TArray<FVector,TInlineAllocator<4>> RejectedErrandParking;
    FVector ErrandParkingWalkGoal=FVector::ZeroVector;
    double RejectedErrandParkingUntil=0.;
    FVector RejectedFollowParkingOwnerGoal=FVector::ZeroVector;
    double RejectedFollowParkingUntil=0;
    double NextVehicleNavigationAt=0;
    // 暂时步行只作用于当前跟随命令和主人 Pawn，保留主人原先选择的骑车方式
    int32 FollowOnFootRevision=INDEX_NONE;
    struct FDeferredIndoorFollow
    {
        TWeakObjectPtr<APlayerState> Owner;
        int32 Revision=INDEX_NONE;
    } DeferredIndoorFollow;
    double FollowVehicleBlockedAt=-1.0;
    FVector FollowVehicleBlockedLocation=FVector::ZeroVector;
    // 同一次骑车路线失败只给仍由主人驾驶的原车一次提前重算机会
    struct FFollowVehicleRoutePause
    {
        TWeakObjectPtr<ADeliveryMotorbike> OwnerBike;
        FVector OwnerGoal=FVector::ZeroVector;
        int32 Revision=INDEX_NONE;
        double ReportedAt=0;
        double RetryAt=0;
        bool bUsedEarlyRetry=false;
    } FollowVehicleRoutePause;
    // 保留旧反射名，实际为唯一单手持物槽；任务查询仅返回其中的 Package 子类
    // 只有服务器交接接口改写，取消安排不会删除实际携带物
    UPROPERTY(ReplicatedUsing=OnRep_CarriedPackage) TObjectPtr<ADeliveryHandheldItem> CarriedPackage;
    UPROPERTY(Replicated) FDeliveryCompanionErrand Errand;
    FAIRequestID ErrandMoveId=FAIRequestID::InvalidRequest;
    void WatchDeliveryApproach(UPathFollowingComponent& Path,FAIRequestID MoveId,AActor& Target);
    void ClearDeliveryApproachWatch();
    TWeakObjectPtr<UPathFollowingComponent> DeliveryApproachPath;
    FDelegateHandle DeliveryApproachFinishedHandle;
    TWeakObjectPtr<UDeliveryTargetComponent> ErrandDeliveryTarget;
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
    double NextCargoHandoffAt=0.;
    FGuid ErrandCargoHandoffId;
    // 暂停时保存车辆已确认的成功，等同一委托的执行分支读取
    bool bErrandCargoHandoffCommitted=false;
    bool HasCommittedPackageHandoff() const;
    void CancelPackageHandoff();
    bool bErrandOwnsNavigation=false;
    bool bHasErrandDestination=false;
    TWeakObjectPtr<UDeliveryTaskManagerComponent> BoundTaskManager;
    FVector LastDestination=FVector::ZeroVector;
    FVector LastOwnerPosition=FVector::ZeroVector;
    FVector WaitLocation=FVector::ZeroVector;
    bool CapturePendingWaitLocation();
    // 跑腿后的返程持续到同层落脚，步行段不依赖车辆行程状态
    bool bReturningToWaitLocation=false;
    // 下车点受阻时，在实际落脚后记录等候位置
    bool bCaptureWaitLocationOnFoot=false;
    double NextNavigationAt=0;
    bool bOwnsNavigation=false;
    FAIRequestID FollowMoveId=FAIRequestID::InvalidRequest;
    // 只监听当前步行跟随的导航请求，旧编号和旧控制对象不参加恢复
    TWeakObjectPtr<UPathFollowingComponent> FollowMovePath;
    FDelegateHandle FollowMoveFinishedHandle;
    TWeakObjectPtr<APawn> FollowMoveOwnerPawn;
    int32 FollowMoveRevision=INDEX_NONE;
    struct FFollowObstructionSample
    {
        TWeakObjectPtr<UPrimitiveComponent> Blocker;
        FVector Feet=FVector::ZeroVector;
        uint32 PathIndex=MAX_uint32;
        double Since=0;
    } FollowObstructionSample;
    TArray<FVector,TInlineAllocator<4>> RejectedFollowStands;
    TWeakObjectPtr<APawn> RejectedFollowOwnerPawn;
    FVector RejectedFollowOwnerPosition=FVector::ZeroVector;
    FVector FollowBlockedFeet=FVector::ZeroVector;
    int32 RejectedFollowRevision=INDEX_NONE;
    double RejectedFollowUntil=0;
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
