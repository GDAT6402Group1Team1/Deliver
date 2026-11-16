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

/**
 * @brief 二狗的基本模式：未结识、跟随、等候、回家
 */
UENUM(BlueprintType)
enum class EDeliveryCompanionMode : uint8 { Unrecruited, Follow, Stay, GoHome };

/**
 * @brief 跑腿委托当前在哪一段：去取件、接主人的包裹、去送件、交回主人、等主人、等空位、在附近找东西
 */
UENUM(BlueprintType)
enum class EDeliveryCompanionErrandPhase : uint8 { Idle, GoToPickup, ReceiveFromOwner, GoToDelivery, ReturnToOwner, WaitingForOwner, WaitingForCapacity, SearchNearby };

/**
 * @brief 同一时刻只有一种安排可以拥有走向车辆和驾驶请求
 */
enum class EDeliveryCompanionTravelPurpose : uint8 { None, Errand, Follow, ReturnToStay, GoHome };

/**
 * @brief 委托与原来的跟随/等候模式分开，短暂倾听和受击不会把已接安排清掉
 */
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryCompanionErrand
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcAction Action=EDeliveryNpcAction::None;
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcActionStatus Status=EDeliveryNpcActionStatus::Idle;
    UPROPERTY(BlueprintReadOnly) EDeliveryCompanionErrandPhase Phase=EDeliveryCompanionErrandPhase::Idle;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<UDeliveryTaskDefinition> Task;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<ADeliveryPackageItem> Package;
    /// 普通物品的安排目标，不代表实际持有，实际持物仍只有 CarriedPackage 一个槽
    UPROPERTY(BlueprintReadOnly) TWeakObjectPtr<ADeliveryHandheldItem> Item;
    /// 服务器封存本次转出方与用途目标，客户端不能用这些引用改交接对象
    TWeakObjectPtr<AActor> ItemSourceCarrier;
    TWeakObjectPtr<ADeliveryPackageItem> ItemUsePackage;
    FName PrankInventorySource;
    bool bInsertIntoHeldPackage=false;
    bool bPrankInsertionCommitted=false;
    /// 就近取还在本次委托里按步行执行，保留主人的远行设置
    bool bLocalItemTransfer=false;
    /// 锁定原件来源，地上包裹失效后不能退回取件点生成另一件
    bool bRecoverGroundPackage=false;
    UPROPERTY(BlueprintReadOnly) int32 CommandRevision=0;
    UPROPERTY(BlueprintReadOnly) FName Reason;
    /// 只有服务器确认在地面线索旁停稳后才开始复制查看姿势
    UPROPERTY(BlueprintReadOnly) double LowItemInspectionServerTime=-1;
    /// 服务器开始伸手的时间，取物成功后保留到起身结束
    UPROPERTY(BlueprintReadOnly) double GroundPickupServerTime=-1;
    /// 服务器在安全落点旁开始放件，实物释放仍由原物品接口确认
    UPROPERTY(BlueprintReadOnly) double GroundPlacementServerTime=-1;
};

/**
 * @brief 服务器保存主人和持续安排，行为树是导航的唯一调用者
 * 对话只改变模式，倾听、受击与乘车暂停不会清掉主人的安排
 */
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
    /// @brief 组件要复制，服务器每 0.25 秒更新一次
    UDeliveryCompanionComponent();
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<UDeliveryCompanionTuning> Tuning;
    UPROPERTY(Replicated,EditInstanceOnly,BlueprintReadOnly,Category="Delivery|同伴|车辆") TObjectPtr<ADeliveryCompanionMotorbike> ErrandVehicle;
    /// @brief 二狗正坐在登记给它的专属车上
    bool IsUsingErrandVehicle() const;
    /// @brief 骑车跟随主人时返回二狗的专属车，否则返回空
    ADeliveryCompanionMotorbike* GetFollowMotorbike() const;
    /// @brief 主人当前要求的出行方式
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|车辆") EDeliveryCompanionTravelMode GetTravelMode() const { return TravelMode; }
    /// @brief 把当前出行方式和可选的出行方式写进模型请求
    void ExtendTravelDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    /// @brief 二狗在车上时请求下车，已经步行时直接返回 true
    bool EnsureOnFoot();
    /// @brief ASC 完成初始化后由角色调用，只授予本组件负责的家居能力
    void InitializeHomeAbility();
    /// @brief 复制下来的家居安排状态
    const FDeliveryCompanionHomeState& GetHomeState() const { return HomeState; }
    /// @brief 有家居安排且还在进行中的阶段
    bool HasActiveHomeArrangement() const;
    /// @brief Source 就是二狗正在搬的家具，搬运编号和占用都还有效
    bool OwnsHomeSource(const UDeliveryHomeObjectComponent* Source) const;
    /// @brief 接收模型选的家居候选：起身直接处理，正在坐着时先排队等起身后再开始
    bool ApplyHomeChoice(const FDeliveryCompanionHomeChoice& Choice,APlayerState* Requester,int32 ExpectedRevision);
    /// @brief 模型选的家居候选仍对应本次请求、当前主人，且二狗空着手、房屋已解锁时返回 true
    bool CanApplyHomeChoice(const FDeliveryCompanionHomeChoice& Choice,APlayerState* Requester,int32 ExpectedRevision) const;
    /// @brief 把共用房屋里能使用、搬动、摆放的家具和记得的家具写进模型请求
    void ExtendHomeDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    /// @brief BT 持续分支调用，普通中断仅暂停本组件自己的路径
    void UpdateHomeArrangement();
    /// @brief 取消家居导航，只停止本组件自己发起的那次移动请求
    void StopHomeMovement();
    /// @brief 取消家居安排；bForce 为 false 且坐着时先起身，否则立即结束
    void CancelHomeArrangement(FName Reason,bool bForce=false);
    /// @brief 家居技能开始执行时调用：取家具时开始搬运，其他操作占用目的家具并进入使用阶段
    virtual bool StartHomeUse(const UDeliveryHomeUseRequest& Request) override;
    /// @brief ArrangementId 是当前安排时调用 CancelHomeArrangement
    virtual void CancelHomeUse(FGuid ArrangementId,FName Reason) override;
    /// @brief 家居使用结束时广播的委托
    virtual FDeliveryHomeUseFinished& GetHomeUseFinishedDelegate() override { return HomeUseFinished; }
    /// @brief 可抓取组件每帧查询：家具应该在的位置和朝向
    virtual bool GetObjectCarryPose(const AActor* Object,const FGuid& CarryId,FVector& OutLocation,FVector& OutForward) const override;
    /// @brief 二狗正在把这件家具放到目的家具上时返回 true
    virtual bool IsObjectCarryInPlacement(const AActor* Object,const FGuid& CarryId) const override;
    /// @brief 可抓取组件通知家具被放开：放置中的按完成处理，其他情况结束安排
    virtual void OnObjectCarryReleased(AActor* Object,const FGuid& CarryId,EDeliveryObjectCarryReleaseReason Reason) override;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|能力") TSubclassOf<UGA_UseHomeObject> HomeUseAbilityClass;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|家居|搬运") TObjectPtr<UAnimSequence> PropCarryAnimation;
    /// 留空使用角色胶囊的实时位置，指定骨骼时偏移按该骨骼坐标且不继承网格缩放
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
    /// @brief 当前主人的 PlayerState，没有主人时为空
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") APlayerState* GetOwnerPlayerState() const { return OwnerPlayerState; }
    /// @brief 当前模式
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") EDeliveryCompanionMode GetMode() const { return Mode; }
    /// @brief 只用已经在地面登记的等候点计算余量
    float GetWaitAreaRemaining() const;
    /// @brief 命令编号，每接一次新安排加一，模型回复的编号要和它对得上
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") int32 GetCommandRevision() const { return CommandRevision; }
    /// @brief 当前暂停原因，没有暂停时为空
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") FName GetSuspensionReason() const { return SuspensionReason; }
    /// @brief 和主人的关系值
    UFUNCTION(BlueprintPure,Category="Delivery|同伴") int32 GetAffinity() const { return Affinity; }
    /// @brief 当前护主目标
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|战斗") AActor* GetAssistTarget() const { return AssistTarget; }
    /// @brief 协战编号，护主目标或开打状态变化时加一
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|战斗") int32 GetAssistRevision() const { return AssistRevision; }
    /// @brief 已经加入协战
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|战斗") bool HasCommittedAssist() const { return bAssistCommitted; }
    /// 主人真实动手或挨打后直接加入协战；关闭后改由模型从 JoinAssist 候选里选择
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|同伴|战斗") bool bJoinAssistWithoutModel=true;
    /// @brief 建立护主目标的那次命中的编号
    FGuid GetAssistEvidenceId() const { return AssistEvidenceId; }
    /// @brief 还没开打且命中记录仍有效时可以加入协战；Requester 为空时表示规则自动加入，主人喊过停手的对手不再自动加入
    bool CanJoinAssist(APlayerState* Requester) const;
    /// @brief 已经在协战、请求人是当前主人且命中记录仍有效时可以收手
    bool CanStopAssist(APlayerState* Requester) const;
    /// @brief 编号对得上且 CanJoinAssist 通过后标记开打，AssistRevision 加一并立即同步
    bool JoinAssist(APlayerState* Requester,int32 ExpectedAssistRevision);
    /// @brief 编号对得上且 CanStopAssist 通过后记下这个对手，再调用 CancelAssist 停止协战
    bool StopAssist(APlayerState* Requester,int32 ExpectedAssistRevision);
    /// @brief 这次命中记录（EvidenceId）仍对应当前护主目标、主人仍在线且目标仍然合法时返回 true
    bool IsAssistEvidenceCurrent(const AActor* Target,FGuid EvidenceId) const;
    /// @brief 主人开口说话时暂停当前这一轮出手，协战安排保留
    void PauseAssistForOwnerConversation(APlayerState* Requester);
    /// @brief 只检查当前真实命中建立的护主目标，模型和访客不能写入这个版本
    bool CanExecuteAssist(AActor* Target,int32 RuleRevision) const;
    /// @brief 协战的接近距离，限制在 200～1200 cm
    float GetAssistRange() const;
    /// @brief 按关系值 Affinity 返回关系阶段的文字，没有主人时是未结识
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|关系") FString GetRelationshipStage() const;
    bool CanChangeSpeechPreference(APlayerState* Requester) const;
    /// @brief 编号对得上时设置或取消安静模式，时长取 QuietSpeechSeconds（15～180 秒）
    bool ApplySpeechPreference(APlayerState* Requester,int32 ExpectedCommandRevision,EDeliveryCompanionSpeechPreference Preference);
    /// @brief 安静模式还剩几秒，取整后限制在 0～180
    int32 GetQuietSecondsRemaining() const;
    /// @brief 服务器当前归属内的真实累计次数，解除主人时清空；战斗扶起不包含首次结识
    int32 GetAssistedRecoveryCount() const { return AssistedRecoveryCount; }
    /// @brief 和主人一起送达的次数
    int32 GetDeliveryTogetherCount() const { return DeliveryTogetherCount; }
    /// @brief 主人打中二狗的次数
    int32 GetOwnerPunchCount() const { return OwnerPunchCount; }
    /// @brief 二狗倒地后主人又打中它的次数
    int32 GetOwnerDownPunchCount() const { return OwnerDownPunchCount; }
    /// @brief 主人骑车撞倒二狗的次数
    int32 GetOwnerVehicleHitCount() const { return OwnerVehicleHitCount; }
    /// @brief 主人扶起二狗后记一次经历，累计次数加一并提高关系值
    bool RecordAssistedRecovery(FGuid DownEpisode,APawn* Helper);
    /// @brief 只保存服务器直连已送达二狗的街坊话语，原话仍是说话人的说法
    void RecordDeliveredNeighborLine(const FDeliveryNpcMessageHandle& Handle);
    /// @brief 请求人是主人，且手上是初遇那一单的包裹、离收件人还有一段距离时可以提议
    bool CanOfferCaterpillar(APlayerState* Requester) const;
    /// @brief 还没提议过时：有请求人就按请求人检查，没有请求人只认当前院子里的机会
    bool CanRequestCaterpillarOffer(APlayerState* Requester) const;
    /// @brief 已经提议给小绿、主人还没接过
    bool HasCaterpillarOffer() const { return bCaterpillarOffered && !bCaterpillarGiven; }
    /// @brief 已经提议过、主人站在 180 cm 内、背包有空位且二狗手上或院子里有小绿时可以接过
    bool CanAcceptCaterpillar(APawn* Interactor) const;
    /// @brief 只由主人实际交互调用，true 表示原背包接口已收到真实道具
    bool AcceptCaterpillar(APawn* Interactor);
    /// @brief 请求人是主人、二狗站着，且共用房屋有回家点和屋内点
    bool CanGoHome(APlayerState* Requester) const;
    /// @brief 主人骑车把二狗撞倒时记一条经历，同一次倒地只记一次
    void RecordOwnerVehicleHit(AActor* Source);
    /// @brief 二狗倒在门口等人扶、还没有主人、玩家在 240 cm 内且能直接看到二狗时可以结识
    bool CanRecruit(APawn* Helper) const;
    /// @brief 仅由服务器长按交互完成入口调用，true 表示归属已经写入
    bool TryRecruit(APawn* Helper);
    /// @brief Requester 就是当前主人且在同一个世界时返回 true
    bool CanCommand(APlayerState* Requester) const;
    /// @brief 编号必须来自本次模型请求，true 表示安排已接收或原委托继续执行
    bool ApplyCommand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
        UDeliveryTaskDefinition* ExpectedTask=nullptr,ADeliveryPackageItem* ExpectedPackage=nullptr,
        EDeliveryCompanionTravelMode RequestedTravelMode=EDeliveryCompanionTravelMode::Unspecified);
    /// @brief 委托状态是 Running
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|跑腿") bool HasActiveErrand() const;
    UFUNCTION(BlueprintPure,Category="Delivery|同伴|跑腿") FDeliveryCompanionErrand GetErrand() const { return Errand; }
    bool CanArrangeErrand(EDeliveryNpcAction Action,APlayerState* Requester) const;
    /// @brief 行为树切换短动作前检查是否应保留当前委托的移动
    bool CanKeepErrandCommand(const FDeliveryNpcPublicState& State) const;
    /// @brief 已经在跟随且没有其他安排时，主人再说跟着可以沿用当前跟随，不重新排安排
    bool CanKeepFollowCommand(APlayerState* Requester,int32 ExpectedRevision,EDeliveryCompanionTravelMode RequestedTravelMode) const;
    /// @brief 当前委托已经接到原件，包括车辆已确认而行为树尚未推进阶段的情况
    bool HasReceivedErrandPackage() const;
    /// @brief 仅行为树调用，执行移动和已验证的物品接口；暂停只停本任务导航
    void UpdateErrand();
    /// @brief 停止委托的导航和收件路线监听；bKeepVehicleSeat 为 true 时留在车上
    void StopErrandMovement(bool bKeepVehicleSeat=false);
    /// @brief 按 ItemObservationInterval 的间隔扫描附近地上的物品，更新看到的物品列表
    void RefreshItemObservations();
    /// @brief 主人说话时立即检查刚看见的小绿，能放进包裹就记下这次看见
    void RefreshCaterpillarObservationForOwnerRequest(APlayerState* Requester);
    /// @brief 正在主人附近找东西
    bool HasNearbyItemSearch() const { return HasActiveErrand() && Errand.Phase==EDeliveryCompanionErrandPhase::SearchNearby; }
    /// @brief 把二狗手上的物品、主人物品栏和附近物品的候选写进模型请求
    void ExtendItemDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    /// @brief 普通道具的短动作只跟踪当前实见物或同槽实际自持物，不把旧记录当当前位置
    bool CanUseObservedItemTarget(const ADeliveryHandheldItem* Item) const;
    /// @brief 检查能否对 Item 安排取物、还物或找物，小绿交给 CanArrangeCaterpillar
    bool CanArrangeItem(EDeliveryNpcAction Action,ADeliveryHandheldItem* Item,APlayerState* Requester,bool bSearchLead=false) const;
    /// @brief 模型选的物品候选仍对应本次请求的编号、证据和当前主人时返回 true
    bool CanApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision) const;
    /// @brief 主人此刻的角色（骑车时取车上的驾驶员），用来找主人身上的物品
    ADeliveryCharacter* GetOwnerItemCarrier() const;
    /// @brief 物品是主人手上或背包里的普通物品，二狗手上空着、站着且没有家居安排时可以接过来
    bool CanTakeOwnerItem(const ADeliveryHandheldItem* Item) const;
    /// @brief 物品在主人手上返回 OwnerHand，在背包里返回 OwnerBackpack，拿不了返回空
    FName GetCurrentOwnerItemSource(const ADeliveryHandheldItem* Item) const;
    /// @brief 主人这次说话时，把主人五个物品栏里的普通物品写进模型请求作为可取候选
    void AddOwnerInventoryCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    /// @brief 主人同意后检查能否捡起小绿（CollectCaterpillar）或把它放进包裹（InsertCaterpillar）
    bool CanArrangeCaterpillar(EDeliveryNpcAction Action,ADeliveryHandheldItem* Item,APlayerState* Requester,bool bSearchLead=false) const;
    /// @brief Package 是初遇那一单的原包裹，且包裹此刻能接收二狗放进来的小绿
    bool IsCurrentPrankPackage(const ADeliveryPackageItem* Package,APlayerState* Requester,bool bCompanionHoldsPackage=false) const;
    /// @brief 在主人附近找小绿；bSeen 表示二狗能直接看见，看不见时 Point 取小绿的模型中心
    ADeliveryCaterpillarItem* FindCaterpillarForOwnerRequest(FVector& Point,bool& bSeen) const;
    /// @brief 记下看到的物品和位置，第一次看到或隔久了再看到时写一条观察事实
    void RecordVisibleWorldItem(ADeliveryHandheldItem* Item,const FVector& Point,double Now);
    /// @brief 模型请求的证据满 32 条时，删掉一条没有被候选引用的证据给找物线索腾位置
    static bool MakeRoomForItemSearchEvidence(FDeliveryNpcDecisionRequest& Request,bool bPrioritizeCurrentItem=false);
    /// @brief 主人这次说话时把捡小绿、放小绿进包裹的候选写进模型请求
    void AddCaterpillarItemCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    /// @brief 走到包裹旁并把小绿放进去；返回 true 表示这一步还在进行或已完成
    bool UpdateCaterpillarInsertion();
    /// @brief 接收取物、还物、找物命令：能沿用就沿用，否则核对候选后开始新委托
    bool ApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision,
        EDeliveryCompanionTravelMode RequestedTravelMode=EDeliveryCompanionTravelMode::Unspecified);
    /// @brief 物品技能请二狗执行接过或交出时调用：核对仍是当前这次，再调用背包接口真正转移物品
    virtual bool ExecuteHandheldUse(const UDeliveryHandheldUseRequest& Request,FName& Reason) override;
    /// @brief 物品技能结束时记下成功与否和原因，UpdateItemErrand 下一次读取
    virtual void FinishHandheldUse(FGuid UseId,bool bSucceeded,FName Reason) override;
    /// @brief 持物槽里仍属于二狗的那件物品，没有返回空
    ADeliveryHandheldItem* GetCarriedItem() const;
    /// @brief 二狗手上空着、没有家居安排，且这件普通物品允许转给二狗
    bool CanReceiveCarriedItem(ADeliveryHandheldItem* Item) const;
    /// @brief 服务器检查 CanReceiveCarriedItem 后调用 StoreCarriedItem 放进持物槽
    bool TryReceiveCarriedItem(ADeliveryHandheldItem* Item);
    /// @brief 清空持物槽和所有者，解绑销毁事件；挂载表现由接收方或掉落流程负责
    bool ReleaseCarriedItem(ADeliveryHandheldItem* Item);
    /// @brief 二狗手上的东西是任务包裹时返回它
    ADeliveryPackageItem* GetCarriedTaskPackage() const;
    /// @brief 二狗能拿包裹、手上空着、没有家居安排，且包裹允许被二狗取走
    bool CanReceiveTaskPackage(ADeliveryPackageItem* Package) const;
    /// @brief 服务器把包裹放进二狗唯一的持物槽，再按任务状态登记取件，不重复接单
    bool TryReceiveTaskPackage(ADeliveryPackageItem* Package);
    /// @brief 二狗手上拿的就是 Package 时释放它
    bool ReleaseTaskPackage(ADeliveryPackageItem* Package);
    /// @brief 在二狗和主人之间找一个有地面、放得下包裹的落点
    bool FindPackageGroundLocation(const ADeliveryPackageItem& Package,const APawn& Receiver,FVector& OutLocation) const;
    /// @brief 把手上的包裹放到主人身边的地上；失败时 Failure 写入原因
    bool TryPlacePackageOnGround(FName& Failure);
    /// @brief 把主人名字、关系阶段、记忆、模式、委托、协战和可选命令写进模型请求
    void ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    /// @brief 只允许 BT 的持续分支调用，其他分支执行时先 StopFollowMovement
    void UpdateFollow();
    /// @brief 停止跟随导航；bKeepVehicleSeat 为 true 且仍可坐车时留在车上，否则先下车
    void StopFollowMovement(bool bKeepVehicleSeat=false);
    /// @brief 对话或受击暂停期间，二狗正在骑专属车出行时可以继续坐在车上
    bool CanKeepVehicleSeatWhilePaused() const;
    /// @brief 由发送查询的拥有者PC转交结果，只记录可见性，不在RPC中移动角色
    void AcceptCatchUpVisibility(APlayerController* Player,FGuid RequestId,APawn* ViewPawn,AActor* ViewTarget,bool bUnseen);
    /// @brief 实例上填了 Tuning 就用它，否则用类默认的调参
    const UDeliveryCompanionTuning& GetTuning() const;
    /// @brief 复制主人、模式、命令编号、暂停原因、关系、委托和协战等状态
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    /// @brief 客户端关掉 Tick；服务器订阅任务完成事件，两端都更新座位表现
    virtual void BeginPlay() override;
    /// @brief 清空跟随监听、记忆、物品和家居安排，解绑任务事件和主人战斗事件
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    /// @brief 服务器每 0.25 秒检查主人是否还在、订阅任务事件，并更新记忆、物品观察和协战
    virtual void TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* TickFunction) override;
private:
    /// @brief 拥有这个组件的二狗角色
    ADeliveryCompanionCharacter* GetCompanion() const;
    struct FObservedPublicPlace
    {
        TWeakObjectPtr<UDeliveryLocationComponent> Location;
        FDeliveryNpcObservedPlace View;
    };
    /// @brief 当次读取本世界公开地点，不缓存可见性或创建新行动证据
    void CollectObservedPlaces(TArray<FObservedPublicPlace>& Out) const;
    /// @brief 调用 CollectObservedPlaces 收集此刻看得到的公开地点，写进模型请求
    void ExtendWorldDecisionRequest(FDeliveryNpcDecisionRequest& Request) const;
    /// @brief 沿既有服务器生命周期更新旧见闻，读取请求时只复制并计算观察年龄
    void RefreshPublicPlaceMemories();
    /// @brief 看到一个公开地点时新增或更新它的记忆，同一个地点只保留一条
    void RecordPublicPlaceMemory(const FObservedPublicPlace& Place,double ObservedAt);
    /// @brief 删掉 RememberedPublicPlaces 里已经没有对应记忆的地点
    void PrunePublicPlaceMemoryIndex();
    /// @brief 复制全部记忆给模型请求，地点记忆补上距离这次看到过了多久
    void CopyCompanionMemories(TArray<FDeliveryNpcCompanionMemory>& Out) const;
    /// @brief 每 2 秒检查屋里看得到的家具，最多记 12 件
    void RefreshHomeObjectMemories();
    /// @brief 把这次没在请求里出现、但之前记得的家具写进模型请求，最多 8 件
    void CopyHomeObjectMemories(const ADeliveryHome& Home,FDeliveryNpcDecisionRequest& Request) const;
    /// @brief 已到手的共用房屋里，二狗熟悉每件家具；人在屋内或门口时不用正对着也能安排
    bool KnowsSharedHomeObject(const UDeliveryHomeObjectComponent* Object) const;
    /// @brief 实物到了手前，或已经跟在搬运距离内等过一秒，就可以开始下一段搬放
    bool IsHomePropReadyAtHand(double Now);
    double HomePropHandWaitStartedAt=-1;
    double NextHomePropHandLogAt=0;
    bool bHomePropHandAccepted=false;
    /// 主人面前一直找不到放得下原尺寸的位置时，改为放在自己脚边
    double HomeOwnerPlacementBlockedSince=-1;
    /// 找不到跟随站位时每两秒记一次原因，查询本身不改变安排
    mutable double NextFollowBlockedLogAt=0;
    /// 跟随找不到可走的站位时，直接跳到主人身边；翻门槛、下台阶这类导航接不上的落差靠它
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随") bool bLeapToOwnerWhenFollowBlocked=true;
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随",meta=(ClampMin="150",ClampMax="1200",Units="cm")) float FollowLeapMaxDistance=750.f;
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随",meta=(ClampMin="50",ClampMax="800",Units="cm")) float FollowLeapMaxDrop=450.f;
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随",meta=(ClampMin="0",ClampMax="150",Units="cm")) float FollowLeapMaxRise=90.f;
    /// @brief 跟随找不到站位时跳到主人身边：检查距离、落点地面和抛物线上没有墙或关着的门
    bool TryLeapToOwner(APawn& OwnerPawn,double Now);
    /// 走不到也跳不到的时间超过这个数，直接出现在主人身边；主人在哪都能把二狗叫到跟前
    UPROPERTY(EditAnywhere,Category="Delivery|Companion|跟随",meta=(ClampMin="2",ClampMax="60",Units="s")) float FollowTeleportAfterSeconds=8.f;
    /// @brief 跟丢主人且没人看见时，把二狗瞬移到主人附近的空站位
    bool TeleportToOwner(APawn& OwnerPawn);
    double FollowStuckSince=-1;
    /// 家居安排途中一直没有路时，过几秒直接到目标站位，和跟随时找不到路的处理一致
    double HomePathBlockedSince=-1;
    FVector HomePathBlockedGoal=FVector::ZeroVector;
    double FollowBlockedSince=-1;
    double NextFollowLeapAt=0;
    /// @brief 主人当前控制的 Pawn，主人离线或不在同一世界时返回空
    APawn* GetOwnerPawn() const;
    /// @brief 在主人周围找一个能走到、不挡任务入口、在主人这一侧的跟随站位
    bool FindFollowDestination(APawn* Pawn,FVector& Out,TFunctionRef<bool(const FVector&)> IsOnOwnerSide) const;
    /// @brief 主人和二狗之间隔着一道开着的门时返回 true
    bool IsAcrossOpenFollowDoorway(const APawn* Pawn,const FVector& FromFeet) const;
    /// @brief 门关着且挡在二狗前面时返回 true，OutStreetAnchor 写入门外的等候点
    bool IsAcrossClosedFollowDoorway(const APawn* Pawn,const FVector& FromFeet,FVector* OutStreetAnchor=nullptr) const;
    /// @brief 跑腿结束后，如果主人在屋里且要求骑车跟随，先把二狗交还步行跟随
    void ResumeIndoorFollowAfterErrand();
    /// @brief 委托正在送货或交回主人，且二狗手上拿的就是这单的原包裹
    bool HasErrandPackageInTransit() const;
    /// @brief 取消跟随导航的监听，只停止本组件自己发起的那次移动请求
    void StopFollowNavigation();
    /// @brief 解绑跟随导航的完成回调并清空监听记录，可以重复调用
    void ClearFollowMoveWatch();
    /// @brief 记下这次跟随导航，订阅完成回调；路上被挡住时记下这个站位，下次不再选它
    void WatchFollowMove(UPathFollowingComponent& Path,FAIRequestID MoveId,APawn& FollowedPawn,const FVector& Destination,bool bWaitingAtDoor=false);
    /// @brief 跟随步行途中检查前方有没有东西挡路，返回挡路的组件，没有返回空
    UPrimitiveComponent* SampleFollowMoveObstruction(const UPathFollowingComponent& Path,const APawn& FollowedPawn,double Now);
    /// @brief 把挡住的站位加进 RejectedFollowStands，主人走开或时间过了就整组清空
    void RememberFollowObstruction(APawn& FollowedPawn,const FVector& Destination);
    /// @brief 已有被挡站位记录，并且记录时的命令编号和主人 Pawn 都和现在相同
    bool IsSameFollowObstructionContext(const APawn& FollowedPawn) const;
    /// @brief 同一安排下的挡路记录还没过期，主人也还在记录时的位置附近
    bool HasRecentFollowObstruction(const APawn& FollowedPawn) const;
    /// @brief Stand 离最近一次被挡的某个站位很近时返回 true
    bool IsRejectedFollowStand(const APawn& FollowedPawn,const FVector& Stand) const;
    /// @brief 等候模式下在离主人一段距离的地方找一个站位
    bool FindWaitDestination(APawn* Pawn,FVector& Out) const;
    /// @brief 二狗的胶囊放在 Destination 时不会和别的东西重叠
    bool IsDestinationClear(const FVector& Destination) const;
    /// @brief Destination 离主人当前任务目的地的距离不小于 DestinationClearance
    bool IsOutsideTaskEntrance(const FVector& Destination) const;
    /// @brief 记下所有玩家的视角，调用 SendCatchUpQueries 问他们看不看得见起点和终点
    bool TryBeginCatchUp(APawn* Pawn,const FVector& Destination);
    /// @brief 所有玩家都回复看不见时调用 TeleportToOwner；有人看见、超时或情况变了就取消
    bool UpdateCatchUp(APawn* Pawn);
    /// @brief 离主人够远、目的地空着、不挡任务入口且能走到时，可以把这个点作为补位终点
    bool CanUseCatchUpDestination(APawn* Pawn,const FVector& Destination) const;
    /// @brief 玩家数量、Pawn 和视角目标都和发查询时一样
    bool AreCatchUpViewersCurrent() const;
    /// @brief 生成新的查询编号，给每个玩家发看不看得见的查询，0.6 秒内要回复
    void SendCatchUpQueries();
    /// @brief 清空补位查询，取消检查可见性的暂停原因
    void CancelCatchUp();
    /// @brief 写入新的暂停原因并立即同步，原因没变就不写
    void SetSuspension(FName Reason);
    /// @brief 主人离开或失效时调用：结束物品、家居、扶起、协战和委托，清空归属与记忆，回到未结识
    void ReleaseOwner();
    /// @brief 主人能指挥二狗时订阅主人 ASC 的真实命中事件，主人变了就先 UnbindOwnerCombat 再订阅新的
    void RefreshOwnerCombatSubscription();
    /// @brief 取消对主人 ASC 真实命中事件的订阅，可以重复调用
    void UnbindOwnerCombat();
    /// @brief 主人真实打中别人或被打中时由 ASC 事件调用，记下对手和命中编号，允许时直接加入协战
    void OnOwnerConfirmedHit(FGameplayTag EventTag,const FGameplayEventData* EventData);
    /// @brief 判断 Target 能否作为护主目标：不能是二狗自己、主人、主人的车或同属一个主人的同伴
    bool IsAllowedAssistTarget(const AActor* Target) const;
    /// @brief 跟随和等候的主体：判断骑车或步行、找站位、发出导航，找不到站位太久时调用 TryLeapToOwner
    void UpdateFollowMovement();
    /// @brief 停止交回主人时的跟随移动
    void StopOwnerHandoffFollow();
    /// @brief 服务器每帧检查护主目标和命中记录是否仍有效，失效就结束协战，有效就保持出手动作
    void UpdateAssist();
    /// @brief 结束协战并清空护主目标；主人给了新安排时把对手记作喊过停手
    void CancelAssist(FName Reason);
    /// @brief 护主目标被销毁时调用 CancelAssist 结束协战
    UFUNCTION() void OnAssistTargetDestroyed(AActor* DestroyedActor);
    /// @brief true 表示已经可以执行末段步行，false 表示仍在取车、骑行或等待安全下车
    bool UpdateVehicleTravel(const FVector& Goal,float ReachRadius,EDeliveryCompanionTravelPurpose Purpose,
        const FVector* RequiredWalkGoal=nullptr);
    /// @brief 停止 Purpose 对应的骑行，跟随骑行时清空跟随路线
    void StopVehicleTravel(EDeliveryCompanionTravelPurpose Purpose,bool bKeepVehicleSeat=false);
    /// @brief Feet 这个落脚点空着，且从这里能上二狗的专属车
    bool IsVehicleBoardingSpotUsable(const FVector& Feet) const;
    /// @brief 走向车的导航还在进行时继续沿用，落脚点被占时重新选一个
    bool KeepVehicleApproach(UPathFollowingComponent& Path,double Now);
    /// @brief 取消走向车的导航，只停止本组件自己发起的那次请求
    void StopVehicleApproach();
    /// @brief 骑车接管导航前先清掉跟随和委托自己的步行导航
    void StopWalkingForVehicle();
    /// @brief 主人骑车时按间隔记下车的位置，二狗沿这条路线骑车跟随
    void SampleFollowBikeRoute(ADeliveryMotorbike* Bike);
    /// @brief 骑行暂停时停下；跟随路线走不通时改为直接追主人的车
    void ReportVehiclePause(FName Reason);
    /// @brief 放弃专属车：清空出行用途，在车上就请求下车
    void ReleaseErrandVehicle();
    /// @brief 服务器接收新安排时记下主人指定的出行方式，Unspecified 时保留上次的选择
    void ApplyTravelInstruction(EDeliveryCompanionTravelMode NewMode);
    /// @brief 跟随模式下主人在骑车且要求骑车跟随时返回 true
    bool ShouldRideForFollow(const APawn* OwnerPawn) const;
    /// @brief 主人就在附近时，接过或交还包裹改为步行就近完成
    bool CanTransferNearbyPackage(EDeliveryNpcAction Action) const;
    /// @brief 物品离主人很近时这次取物改为步行，不用骑车
    bool CanWalkForNearbyItem(const FDeliveryCompanionItemChoice& Choice) const;
    UPROPERTY(Replicated) EDeliveryCompanionTravelMode TravelMode=EDeliveryCompanionTravelMode::Unspecified;
    /// 未指定时跟随主人当前的步行或骑乘，跑腿的出行选择独立
    EDeliveryCompanionTravelMode FollowTravelMode=EDeliveryCompanionTravelMode::Unspecified;
    /// @brief 按事件编号 EventId 去重后新增一条记忆并调整关系值 AffinityDelta，达到上限后只报告一次不再新增
    bool RecordMemory(FGuid EventId,FName Kind,const FString& Description,int32 AffinityDelta);
    /// @brief 结识时记下被主人救起这件事，已经有这条就不再记
    void RecordRecruitMemory();
    /// @brief 主人交付任务时二狗在附近且看得见，记一次一起送货的经历，同一任务只记一次
    bool RecordDeliveryMemory(UDeliveryTaskDefinition* Task,APlayerState* Deliverer,bool bDeliveredByCompanion);
    /// @brief 主人打中二狗时记一条经历并降低关系值，二狗已经倒地时扣得更多
    void RecordOwnerPunch(FGuid PunchId,bool bWasDown);
    /// @brief 清空所有记忆、去重记录和累计次数，解除主人时调用
    void ClearCompanionMemories();
    /// @brief 回家模式下导航回共用房屋，对话或其他动作期间先停下
    void UpdateGoHome();
    /// @brief 按操作类型占用来源和目的家具，开始一次新的家居安排
    bool BeginHomeChoice(const FDeliveryCompanionHomeChoice& Choice,FGuid ArrangementId,FName& OutReason);
    /// @brief 在主人面前找一个能放下这件家具原尺寸的位置，Entrance 是二狗站的位置，Placement 是家具的位置
    bool ResolveHomeOwnerPlacement(const UDeliveryHomeObjectComponent& Source,const APawn* OwnerPawn,
        FTransform& Entrance,FTransform& Placement) const;
    /// @brief GroundPoint 在主人身前、高度差不大、距离合适时返回 true
    bool IsHomePlacementInFrontOfOwner(const APawn* Pawn,const FVector& GroundPoint,const AActor* Prop) const;
    /// @brief 搬着家具走到主人面前的落点，主人走开或换了人就结束安排
    void UpdateHomeOwnerPlacementApproach(FGuid ArrangementId,double Now);
    /// @brief 放置家具的过程：放开后等它停稳，超时或被拿走就失败
    void UpdateHomePropPlacement(FGuid ArrangementId,double Now,UDeliveryHomeObjectComponent* Destination=nullptr);
    /// @brief 家居导航入口：到了 Goal 附近返回 true，否则发出导航请求
    bool MoveForHome(const FVector& Goal,float Radius);
    /// @brief 转向家具的使用方向，坐姿移动中由座位过程负责
    bool AlignForHome(const FTransform& Use);
    /// @brief 切换家居阶段，重置这一阶段的计时和导航间隔
    void SetHomePhase(EDeliveryCompanionHomePhase Phase);
    /// @brief 设置要复制的家居姿势动画、是否循环、是否只用上半身，并记下开始时间
    void SetHomePose(UAnimSequence* Animation,bool bLoop,bool bUpperBody);
    /// @brief 手上有家具就播搬运姿势，没有就清空姿势
    void RefreshHomeCarryPose();
    /// @brief 服务器每帧检查家居安排还能否继续，并开始排队中的安排
    void TickHomeLifecycle();
    /// @brief 组件结束时强制取消家居安排、座位移动，收回家居技能
    void ShutdownHomeExecution();
    /// @brief 结束家居安排：释放占用和家具，记录结果并通知模型
    void FinishHomeArrangement(EDeliveryCompanionHomePhase Phase,FName Reason,bool bKeepProp=false);
    /// @brief 进入起身阶段，播放家具的退出动作
    void BeginLeavingHomeUse(FName Reason);
    /// @brief 释放家具的占用并解绑使用结束事件
    void ReleaseHomeClaim(FDeliveryHomeUseClaim& Claim,FName Reason);
    /// @brief 放开正在搬的家具并释放来源占用
    void ReleaseHomeProp(FName Reason);
    /// @brief 开始搬起占用的家具，失败时 OutReason 写入原因
    bool BeginHomePropCarry(FName& OutReason);
    /// @brief 搬运家具时手前的目标位置和朝向，由 PropCarrySocket 和 PropCarryOffset 决定
    bool GetHomeHandPose(FVector& OutLocation,FVector& OutForward) const;
    /// @brief 家具已经到了手前位置并转到了手前朝向
    bool IsHomePropAtHand() const;
    /// @brief 搬运要经过门洞交接点时检查交接点仍然有效
    bool ValidateHomePropTransfer(FName& OutReason) const;
    /// @brief 清空门洞交接点
    void ClearHomePropTransfer();
    /// @brief 开始新的一段搬运，从家具当前位置平滑移到手前或交接点
    void BeginHomePropSegment(bool bToTransferPoint);
    /// @brief 家具已经到了门洞交接点
    bool HasReachedHomePropTransferPoint() const;
    /// @brief 家具离手前位置超过 PropCarryPauseDistance 时返回 true
    bool IsHomePropLagging() const;
    /// @brief ArrangementId 仍是当前进行中的家居安排，且组件没有在结束
    bool IsCurrentHome(FGuid ArrangementId) const;
    /// @brief 坐下之后结束安排前要先播起身动作
    bool NeedsHomeExit() const;
    /// @brief 起身结束后开始排队中的家居安排，过期就放弃
    void StartPendingHomeChoice();
    /// @brief 家具使用被别处结束时结束当前家居安排或放开家具
    void OnHomeObjectUseEnded(FGuid ActionId,FName Reason);
    /// @brief 家居动作用的服务器时间
    double GetHomeServerTime() const;
    /// @brief 客户端收到家居状态时更新座位表现
    UFUNCTION() void OnRep_HomeState();
    void UpdateHomeSeatPresentation();
    /// @brief 开始坐下或起身的移动，时长取动画长度；失败时 OutReason 写入原因
    bool BeginHomeSeatMove(const FTransform& FeetGoal,UAnimSequence* Animation,FName& OutReason);
    /// @brief 检查坐下或起身的移动是否还对应当前安排，移动结束时返回 true
    bool UpdateHomeSeatMove(const FTransform& FeetGoal,FName& OutReason);
    /// @brief 去掉坐下或起身的根运动并恢复移动设置；bClearState 为 true 时服务器同时清空复制的座位状态
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
    /// 中转点在当前家具坐标上冻结，首段保持实物开始时的朝向
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
    /// @brief 主人带着初遇那一单走到院子附近时生成一次提议小绿的机会，离开就作废
    void UpdateCaterpillarOpportunity();
    /// @brief 院子里发现小绿的机会还没过期、还没提议过，且主人就在二狗身边
    bool HasCaterpillarOpportunity() const;
    /// @brief 在 Pawn 周围 Radius 内找一只登记在交互注册表里、看得见的院子小绿
    ADeliveryCaterpillarItem* FindReachableYardCaterpillar(const APawn* Pawn,float Radius) const;
    /// @brief 把物品写进持物槽、设所有者为二狗、订阅销毁事件，并刷新挂载表现
    bool StoreCarriedItem(ADeliveryHandheldItem* Item);
    /// @brief 把手上的物品原地放到地上，任务和奖励不变
    void DropCarriedItem();
    /// @brief 二狗能从眼睛位置直接看到地上的物品时返回 true，Point 写入看到的位置
    bool CanSeeWorldItem(const ADeliveryHandheldItem* Item,FVector& ObservedPoint) const;
    /// @brief 主人这次说话时，把附近看到过的普通物品写进模型请求作为可取候选
    void AddNearbyItemSearchCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    /// @brief 主人本人在旁边步行、二狗站着且没有其他安排时，可以在主人周围找东西
    bool CanArrangeOwnerAreaSearch(APlayerState* Requester) const;
    /// @brief 主人这次说话时加入在主人周围找东西的候选
    void AddOwnerAreaSearchCandidate(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const;
    /// @brief 以 Origin 为中心重新开始找物路线，清空已看过的点
    void ResetItemSearchRoute(const FVector& Origin,bool bAlreadyOnFoot);
    TArray<FString> AreaSearchSeenNames;
    bool bAreaSearchReachedStand=false;
    /// @brief 让二狗转头看向 Point，Angle 是在水平方向上额外偏转的角度
    void SetItemSearchFocus(const FVector& Point,float Angle);
    enum class EGroundPickupProgress : uint8 { Unavailable, Preparing, AtContact };
    /// @brief 捡地上物品的过程：先转向对准，再伸手；bInReach 表示已经走到能伸手的位置
    EGroundPickupProgress UpdateGroundPickup(ADeliveryHandheldItem* Item,bool bInReach,TFunctionRef<bool()> IsCurrent);
    /// @brief 行为树每帧推进取物委托：找物、走过去、捡起或接过、交还主人
    void UpdateItemErrand();
    /// @brief 授予并激活物品转移技能，bReturnToOwner 为 true 时是交给主人；失败时 Reason 写入原因
    bool RunItemTransfer(bool bReturnToOwner,FName& Reason);
    /// @brief UseId、命令编号和物品都还对应当前这次取物或放物时返回 true
    bool IsCurrentItemUse(FGuid UseId,int32 Revision,ADeliveryHandheldItem* Item) const;
    /// @brief 作废当前物品使用编号，正在执行的物品技能一起取消
    void CancelItemUse();
    /// @brief 取消物品使用、收回物品技能，清空看到的物品和手上物品的记录
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
    /// @brief 这次命令对应的任务：有包裹按包裹的任务，没有就取主人正在追踪的任务
    UDeliveryTaskDefinition* GetCommandTask() const;
    /// @brief 根据二狗亲眼看到的情况判断收件人家门是开着、关着还是看不到
    EDeliveryCompanionTaskDoorState GetObservedTaskDoorState(const UDeliveryTaskDefinition* Task) const;
    /// @brief 这次命令对应的包裹：先看二狗手上，再看主人身上或车上，最后看主人脚边掉落的包裹
    ADeliveryPackageItem* GetCommandPackage() const;
    /// @brief 主人刚掉在附近地上的任务包裹，没有就返回空
    const ADeliveryPackageItem* GetOwnerDroppedPackage() const;
    static constexpr float GroundPackageReach=150.f;
    /// @brief 走到地上的原包裹旁捡起来；失败时 Failure 写入原因
    bool TryRecoverGroundPackage(FName& Failure);
    /// @brief 主人身上或车上实际拿着包裹的那个 Actor，作为交接时的转出方
    APawn* GetOwnerPackageCarrier() const;
    /// @brief 主人重复下达同一个跑腿命令时可以沿用当前委托，不重新开始
    bool CanKeepPackageErrand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
        UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,EDeliveryCompanionTravelMode RequestedTravelMode) const;
    /// @brief 主人重复确认同一件取物安排时沿用当前委托，保留导航和找物阶段
    bool CanKeepItemErrand(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision,
        EDeliveryCompanionTravelMode RequestedTravelMode) const;
    /// @brief 接收跑腿命令：先看能否沿用当前委托，否则核对任务和包裹后开始新委托并决定出行方式
    bool StartErrand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
        UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,
        EDeliveryCompanionTravelMode RequestedTravelMode=EDeliveryCompanionTravelMode::Unspecified);
    /// @brief 结束委托：取消交接、停下移动，记录结果原因并通知模型这次动作的结果
    void FinishErrand(EDeliveryNpcActionStatus Status,FName Reason);
    /// @brief 停下当前移动后切换委托阶段并立即同步
    void SetErrandPhase(EDeliveryCompanionErrandPhase Phase);
    /// @brief 委托暂停时停下移动；bKeepMounted 为 true 且正在骑车时留在车上
    void ReportErrandPause(FName Reason,bool bKeepMounted=false);
    /// @brief 委托的导航入口：到了 Goal 附近返回 true，否则发出导航请求
    bool MoveForErrand(const FVector& Goal,float ReachRadius,AActor* InteractionTarget,float PreferredOffset=0.f,
        UDeliveryTargetComponent* Delivery=nullptr,const ADeliveryHome* DoorWaitHome=nullptr);
    /// @brief Feet 在收件人家门外的街边，不挡门也不挡主人时返回 true
    bool IsSafeDeliveryDoorWait(const ADeliveryHome& Home,const FVector& Feet,float AcceptanceMargin=0.f) const;
    /// @brief 清空送货时被挡住的站位记录
    void ResetDeliveryStandRejections();
    /// @brief 收件点或参考位置变了就清空被挡站位记录
    void RefreshDeliveryStandRejections(const UDeliveryTargetComponent& Delivery);
    /// @brief 把送货时走不到的站位记下来，最多保留 MaxRejectedDeliveryStands 个
    void RejectDeliveryStand(const UDeliveryTargetComponent& Delivery,const FVector& Stand);
    /// 近期受阻站位绑定实际收件组件和固定参照点
    TArray<FVector,TInlineAllocator<4>> RejectedDeliveryStands;
    TWeakObjectPtr<const UDeliveryTargetComponent> RejectedDeliveryTarget;
    FVector RejectedDeliveryReference=FVector::ZeroVector;
    /// @brief bPickup 为 true 时返回取件点，否则返回收件点
    AActor* ResolveErrandLocation(bool bPickup) const;
    /// @brief 客户端收到持物变化时刷新包裹的挂载表现
    UFUNCTION() void OnRep_CarriedPackage();
    UFUNCTION() void OnCarriedPackageDestroyed(AActor* DestroyedActor);
    void BindTaskCompletion();
    /// @brief 主人交付了二狗参与过的任务时，记一次一起送货的经历
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
    /// 真实现场事件只在等待初次模型选择时到期
    double PendingAssistExpiresAt=0;
    /// 主人叫停后，同一对象的新命中需要主人明确重新加入
    TWeakObjectPtr<AActor> StoppedAssistTarget;
    double AssistStoppedAt=-1;
    /// @brief 只结束当前这一轮由规则发起的出手动作，bAssistCommitted 保持不变
    void PauseAssistRound(FName Reason);
    /// 当前连续交手各方向最多记一次，切目标、过期或取消后清空；仅服务器使用
    uint8 RememberedAssistHitDirections=0;
    TArray<FGuid> RecentAssistHits;
    TWeakObjectPtr<UAbilitySystemComponent> BoundOwnerCombat;
    FDelegateHandle OwnerCombatHandle;
    TArray<FDeliveryNpcCompanionMemory> Memories;
    /// 服务器世界时间的截止点，与移动安排和亲密度分别保存
    double QuietSpeechUntil=0;
    struct FRememberedNeighborLine
    {
        TWeakObjectPtr<ADeliveryNpcCharacter> Speaker;
        FGuid MessageId;
    };
    /// 只索引 Memories 已保存的各街坊最近一句，不保活人物
    TArray<FRememberedNeighborLine> RememberedNeighborLines;
    /// 只索引 Memories 中最多六条地点见闻，不保活地点，也不保存导航坐标
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
    /// 只记本局亲眼见过的名称和用途描述，不保存道具引用或上次位置
    TWeakObjectPtr<ADeliveryHome> ObservedHome;
    TMap<FGuid,FRememberedHomeObject> RememberedHomeObjects;
    double NextHomeObjectObservationAt=0;
    /// 当局去重不随模型最近 24 条记忆淘汰，旧事件重发不能再次加减关系
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
    /// 驾驶目标绑定本段主人 Pawn，轨迹保留主人刚驶过的车位
    TWeakObjectPtr<APawn> VehicleFollowOwnerPawn;
    int32 OwnerHandoffFollowRevision=INDEX_NONE;
    TArray<FVector> FollowRoute;
    /// 主人结束一次真实骑行后保持步行，直到再次骑车或明确改出行方式
    bool bFollowOwnerWasRiding=false;
    bool bFollowWalkAfterOwnerRide=false;
    /// @brief 记下主人是在骑车还是刚下车，二狗据此决定继续骑车还是停车步行
    void RefreshFollowOwnerTravel(const APawn* Pawn);
    /// 跟随的步行出口检查失败后，短期避开同一个停车位置
    TArray<FVector,TInlineAllocator<4>> RejectedFollowParking;
    TArray<FVector,TInlineAllocator<4>> RejectedErrandParking;
    FVector ErrandParkingWalkGoal=FVector::ZeroVector;
    double RejectedErrandParkingUntil=0.;
    FVector RejectedFollowParkingOwnerGoal=FVector::ZeroVector;
    double RejectedFollowParkingUntil=0;
    double NextVehicleNavigationAt=0;
    /// 暂时步行只作用于当前跟随命令和主人 Pawn，保留主人原先选择的骑车方式
    int32 FollowOnFootRevision=INDEX_NONE;
    struct FDeferredIndoorFollow
    {
        TWeakObjectPtr<APlayerState> Owner;
        int32 Revision=INDEX_NONE;
    } DeferredIndoorFollow;
    double FollowVehicleBlockedAt=-1.0;
    FVector FollowVehicleBlockedLocation=FVector::ZeroVector;
    /// 同一次骑车路线失败只给仍由主人驾驶的原车一次提前重算机会
    struct FFollowVehicleRoutePause
    {
        TWeakObjectPtr<ADeliveryMotorbike> OwnerBike;
        FVector OwnerGoal=FVector::ZeroVector;
        int32 Revision=INDEX_NONE;
        double ReportedAt=0;
        double RetryAt=0;
        bool bUsedEarlyRetry=false;
    } FollowVehicleRoutePause;
    /// 保留旧反射名，实际为唯一单手持物槽；任务查询仅返回其中的 Package 子类
    /// 只有服务器交接接口改写，取消安排不会删除实际携带物
    UPROPERTY(ReplicatedUsing=OnRep_CarriedPackage) TObjectPtr<ADeliveryHandheldItem> CarriedPackage;
    UPROPERTY(Replicated) FDeliveryCompanionErrand Errand;
    FAIRequestID ErrandMoveId=FAIRequestID::InvalidRequest;
    /// @brief 订阅送货导航的完成回调，没走到时调用 RejectDeliveryStand 记下这个站位
    void WatchDeliveryApproach(UPathFollowingComponent& Path,FAIRequestID MoveId,AActor& Target);
    /// @brief 解绑送货导航的完成回调
    void ClearDeliveryApproachWatch();
    TWeakObjectPtr<UPathFollowingComponent> DeliveryApproachPath;
    FDelegateHandle DeliveryApproachFinishedHandle;
    TWeakObjectPtr<UDeliveryTargetComponent> ErrandDeliveryTarget;
    FVector ErrandDestination=FVector::ZeroVector;
    FVector ErrandGoal=FVector::ZeroVector;
    double NextErrandNavigationAt=0;
    double LastErrandUpdateAt=0;
    float ErrandActiveSeconds=0;
    /// 委托基础余量加各阶段第一次完整路径的行走时间，不随每次重寻路增长
    double ErrandTimeLimitSeconds=0;
    bool bErrandTravelBudgetAdded=false;
    /// 取回远处真实停车车辆的步行时间与送取路线分开，每阶段只计入一次
    bool bErrandVehicleApproachBudgetAdded=false;
    float ErrandInteractionSeconds=0;
    FName LastErrandPause;
    /// 只取消本次从主人车辆接货的交接，不影响后来开始的装卸
    TWeakObjectPtr<ADeliveryMotorbike> ErrandCargoVehicle;
    double NextCargoHandoffAt=0.;
    FGuid ErrandCargoHandoffId;
    /// 暂停时保存车辆已确认的成功，等同一委托的执行分支读取
    bool bErrandCargoHandoffCommitted=false;
    /// @brief 和车辆的货物交接已经确认完成
    bool HasCommittedPackageHandoff() const;
    /// @brief 取消和车辆的货物交接，清空交接编号
    void CancelPackageHandoff();
    bool bErrandOwnsNavigation=false;
    bool bHasErrandDestination=false;
    TWeakObjectPtr<UDeliveryTaskManagerComponent> BoundTaskManager;
    FVector LastDestination=FVector::ZeroVector;
    FVector LastOwnerPosition=FVector::ZeroVector;
    FVector WaitLocation=FVector::ZeroVector;
    /// @brief 等候安排要求在步行状态下记录等候点时，下车后记下当前位置；不需要记录时直接返回 true
    bool CapturePendingWaitLocation();
    /// 跑腿后的返程持续到同层落脚，步行段不依赖车辆行程状态
    bool bReturningToWaitLocation=false;
    /// 下车点受阻时，在实际落脚后记录等候位置
    bool bCaptureWaitLocationOnFoot=false;
    double NextNavigationAt=0;
    bool bOwnsNavigation=false;
    FAIRequestID FollowMoveId=FAIRequestID::InvalidRequest;
    /// 只监听当前步行跟随的导航请求，旧编号和旧控制对象不参加恢复
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
    /// 两次按需查询属于同一模式版本，任何控制对象变化、超时或拒绝都取消补位
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
