#pragma once
#include "CoreMinimal.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "DeliveryCompanionCharacter.generated.h"
class UDeliveryCompanionComponent;
class ADeliveryCompanionEncounterScene;
class UCurveVector;

/**
 * @brief 初遇的阶段：等待开始、开门、被扔出、下落、等人扶、被扶起、已结识
 */
UENUM(BlueprintType)
enum class EDeliveryCompanionEncounterPhase : uint8
{
    Dormant, OpeningDoor, Ejecting, Falling, AwaitingHelp, Helping, Joined
};

/**
 * @brief 阶段与本次轨迹一并复制，客户端不从主人引用到达顺序推断演出阶段
 */
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

/**
 * @brief 复用居民的交谈与身体表现，初遇交互把归属交给同伴组件
 */
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
    /// @brief 创建二狗组件、包裹挂点、手持物挂点和扶起交互入口
    ADeliveryCompanionCharacter();
    /// 用本机播放调整原音色，1 为原调
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|同伴|语音",meta=(ClampMin="0.95",ClampMax="1.08"))
    float DialoguePitch=1.045f;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<UDeliveryCompanionComponent> Companion;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<UDeliveryInteractableComponent> HelpInteractable;
    /// 挂点由角色蓝图调整，物品继续使用原包裹模型与复制
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<USceneComponent> PackageAnchor;
    /// 普通道具挂实际手骨，锚点始终零位移和 1 倍世界缩放，物品蓝图保存握持偏移
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|同伴") TObjectPtr<USceneComponent> HandheldAnchor;
    /// @brief 二狗手上的普通物品
    virtual ADeliveryHandheldItem* GetCarriedHandheldItem() const override;
    /// @brief 手持物品按主人结算
    virtual APlayerState* GetHandheldOwnerPlayerState() const override;
    /// @brief 已结识、主人在线、没在受击反应时可以拿普通物品
    virtual bool CanHandleHandheldItem() const override;
    /// @brief Item 就在二狗的持物槽里时返回 true
    virtual bool OwnsHandheldItem(const ADeliveryHandheldItem* Item) const override;
    /// @brief 转给组件的 CanReceiveCarriedItem
    virtual bool CanReceiveHandheldItem(ADeliveryHandheldItem* Item) const override;
    /// @brief 转给组件的 TryReceiveCarriedItem
    virtual bool TryReceiveHandheldItem(ADeliveryHandheldItem* Item) override;
    /// @brief 只释放普通物品，转给组件的 ReleaseCarriedItem
    virtual bool ReleaseHandheldItem(ADeliveryHandheldItem* Item) override;
    /// @brief 把二狗手上的物品挂到手上或车上：骑专属车时挂到车的货位，否则挂到手持挂点
    virtual void ApplyHandheldItemPresentation(ADeliveryHandheldItem* Item) override;
    /// @brief 二狗手上的任务包裹，转给组件的 GetCarriedTaskPackage
    virtual ADeliveryPackageItem* GetCarriedTaskPackage() const override;
    /// @brief 二狗代表的玩家就是主人，任务和奖励按主人结算
    virtual APlayerState* GetTaskPlayerState() const override;
    /// @brief 二狗站着、没在骑车、主人在线时可以拿任务包裹
    virtual bool CanHandleTaskPackage() const override;
    /// @brief 转给组件的 CanReceiveTaskPackage
    virtual bool CanReceiveTaskPackage(ADeliveryPackageItem* Package) const override;
    /// @brief 转给组件的 TryReceiveTaskPackage
    virtual bool TryReceiveTaskPackage(ADeliveryPackageItem* Package) override;
    /// @brief 转给组件的 ReleaseTaskPackage
    virtual bool ReleaseTaskPackage(ADeliveryPackageItem* Package) override;
    /// @brief 包裹按普通手持物品的方式挂到二狗身上，转给 ApplyHandheldItemPresentation
    virtual void ApplyTaskPackagePresentation(ADeliveryPackageItem* Package) override;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|初遇") FName EncounterTaskId=TEXT("Task_001");
    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,ReplicatedUsing=OnRep_Encounter,Category="Delivery|初遇") TObjectPtr<ADeliveryCompanionEncounterScene> EncounterScene;
    /// @brief 初遇已经开始，不在等待开始的阶段
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") bool HasEncounterStarted() const { return Encounter.Phase!=EDeliveryCompanionEncounterPhase::Dormant; }
    /// @brief 复制下来的初遇状态
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") FDeliveryCompanionEncounterState GetEncounterState() const { return Encounter; }
    /// @brief 二狗正被扔出或正在下落时返回 true
    bool IsEncounterAirborne() const;
    /// @brief 二狗正坐在登记给它的专属摩托车座位上时返回 true
    bool IsMountedOnCompanionBike() const;
    /// @brief 二狗倒在地上等人扶且还没有主人
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") bool IsWaitingForHelp() const;
    bool HasFinishedEncounterFall() const;
    /// @brief 正处在被扶起的阶段
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") bool IsBeingHelpedUp() const;
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") double GetEncounterStartTime() const { return Encounter.EncounterStartedAt; }
    UFUNCTION(BlueprintPure,Category="Delivery|初遇") double GetEncounterElapsed() const;
    double GetHelpStartTime() const { return IsBeingHelpedUp()?Encounter.PhaseStartedAt:-1; }
    /// @brief 倒地动画开始的服务器时间
    double GetEncounterFallStartTime() const { return Encounter.FallAnimationStartedAt; }
    /// @brief 有人开始扶时从等人扶切到被扶起阶段
    void BeginHelpUp();
    /// @brief 撤销 Joined 后的辅助起身和旧长按，不改变主人或持续安排
    void CancelAssistedHelp();
    /// @brief 失去主人后就在当前位置倒下等新主人扶，不回面馆重演初遇
    void ResetForNewOwner();
    /// @brief 还没结识时一律算倒地，结识后按居民的倒地状态判断
    virtual bool IsKnockedDown() const override;
    /// @brief 在空中被撞时改为下落，其余情况交给居民的受击流程
    virtual void ReceiveImpactFrom(const FVector& Impulse,const FVector& Point,AActor* Source) override;
    /// @brief 服务器记下这一拳的编号和挨打前是否已倒地，再交给居民的受击流程
    virtual void ReceiveMeleeHitFrom(const FVector& Impulse,const FVector& Point,AActor* Source,FGuid PunchId) override;
    /// @brief PunchId 就是最近一拳，且挨这一拳之前二狗已经倒地
    bool WasDownBeforeMeleeHit(FGuid PunchId) const { return PunchId==LastMeleePunchId && bWasDownBeforeMelee; }
    /// @brief 跟随组件负责同伴站位，避免两个导航请求互相替换
    virtual bool CanAdjustStandingPosition() const override { return false; }
    /// @brief 二狗专用动作的执行条件：协战、踱步和同伴命令，其他动作交给居民的判断
    virtual bool CanExecute(EDeliveryNpcAction Action,AActor* Target) const override;
    /// @brief 和 CanExecute 相同，但按 Requester 检查主人权限
    virtual bool CanExecuteForRequester(EDeliveryNpcAction Action,AActor* Target,APlayerState* Requester) const override;
    /// @brief 已批准的踱步还需要在每段导航期间维持原等候安排
    bool CanContinuePace(const FDeliveryNpcPublicState& State) const;
    /// @brief 规则发起的协战出手：只允许 Fight，且护主目标和规则编号都要对得上
    virtual bool CanExecuteGameplayRule(EDeliveryNpcAction Action,AActor* Target,int32 RuleRevision) const override;
    /// @brief 规则发起的协战用协战距离接近对手
    virtual float GetGameplayRuleFightApproachRange() const override;
    /// @brief 按顺序把同伴、家居、物品、出行和协战信息写进模型请求
    virtual void ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const override;
    /// @brief 执行动作前先看是否继续同伴已有的移动安排，踱步要先通过 CanContinuePace
    virtual bool ExecuteAction(const FDeliveryNpcPublicState& State) override;
    /// @brief 被动动作只清掉 PassiveActionId，其他动作交给居民的 StopAction
    virtual void StopAction(FGuid ActionId) override;
    /// @brief 蓝图配置老板、编织袋等初遇表现，服务器时间供迟到客户端接上进度
    UFUNCTION(BlueprintImplementableEvent,Category="Delivery|初遇") void OnEncounterStarted();
    virtual bool IsNetRelevantFor(const AActor* RealViewer,const AActor* ViewTarget,const FVector& SrcLocation) const override;
    /// @brief 复制初遇状态和初遇场景
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    /// @brief 一次踱步能走的距离：受等候区剩余半径和踱步时长限制
    virtual float GetPaceTravelBudget() const override;
    /// @brief 授予家居能力，绑定扶起交互的 CanUse/Use 和战斗组件的倒地、起身事件
    virtual void BeginPlay() override;
    /// @brief 服务器推进初遇流程，客户端播放抛出动作，倒地时检查主人是否还在扶起
    virtual void Tick(float DeltaSeconds) override;
    /// @brief 取消扶起、解绑战斗事件、停止抛出动作并清理初遇场景的表现
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    /// @brief 下落阶段落到地面时调用 ConfirmEncounterLanding
    virtual void Landed(const FHitResult& Hit) override;
    /// @brief 客户端在抛出阶段移动模式变成 None 时记下来，之后重新加上抛出动作
    virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode,uint8 PreviousCustomMode=0) override;
private:
    /// @brief 等候模式下空闲站着、手上没东西也没有其他安排时可以踱步
    bool HasPaceContext() const;
    /// @brief 同一个 E 入口的可用条件：没结识时判断能否结识，倒地时判断能否扶起，站着时判断能否接过小绿
    bool CanHelp(APawn* Helper,bool bCheckAvailability);
    /// @brief 服务器执行 E 入口：结识调用 TryRecruit，倒地调用 StartAssistedRecovery，站着调用 AcceptCaterpillar
    bool Help(APawn* Helper);
    /// @brief 扶起期间每帧检查：同一次倒地、主人本人在旁边、能看见且仍在按住 E
    bool CanContinueAssistedHelp(APawn* Helper,FGuid DownEpisode) const;
    /// @brief 二狗被击倒或倒地又挨打时取消主人旧的按住进度，并刷新交互提示
    void OnCombatDownChanged(FGuid DownEpisode);
    /// @brief 起身完成后刷新交互提示，有人扶起时调用 RecordAssistedRecovery 记下这次经历
    void OnCombatRecoveryFinished(FGuid DownEpisode,APawn* Helper);
    /// @brief 按当前阶段更新 E 入口的提示文字和按住时长
    void UpdateHelpInteraction();
    /// 这个短动作只拥有对白与注视，结束时不能清掉同伴导航
    FGuid PassiveActionId;
    FGuid LastMeleePunchId;
    bool bWasDownBeforeMelee=false;
    /// @brief 按初遇阶段切换表现：骑车时隐藏本体，抛出阶段播放抛出动作，结识后恢复正常
    UFUNCTION() void OnRep_Encounter();
    UPROPERTY(ReplicatedUsing=OnRep_Encounter) FDeliveryCompanionEncounterState Encounter;
    /// @brief 初遇用的服务器时间；客户端还没收到 GameState 时返回 -1
    double GetEncounterServerTime() const;
    /// @brief 服务器切换初遇阶段，记下开始时间、编号加一，并立即同步
    void SetEncounterPhase(EDeliveryCompanionEncounterPhase Phase);
    /// @brief 服务器按初遇阶段推进：等待开始、开门、抛出、下落、等人扶和被扶起
    void UpdateEncounter();
    /// @brief 开门和抛出阶段显示老板，并按门的进度同步老板的动画时间
    void UpdateEncounterPresentation();
    /// @brief 初遇绑定的任务已经开始、进行中或已完成时返回 true
    bool HasEncounterOrderStarted() const;
    /// @brief Pawn 在门口触发范围内，或者是在取餐点附近的玩家
    bool CanWitnessEncounter(const APawn* Pawn) const;
    /// @brief 有玩家在门口附近，触发后的范围放宽，让玩家可以退到门边让开
    bool HasNearbyEncounterWitness() const;
    /// @brief 场景和抛出曲线都配好、任务已开始、有玩家在旁边时开始初遇并请求开门
    bool TryStartEncounter();
    /// @brief 门开好后计算抛出起点、落点和时长，路线通畅就切到抛出阶段，被挡就等待
    void TryStartEjection();
    enum class EEjectionPathStatus : uint8 { Invalid, Blocked, Clear };
    /// @brief 检查抛出路线：参数是否合法、路线上有没有东西挡住
    EEjectionPathStatus GetEjectionPathStatus(const FDeliveryCompanionEncounterState& State) const;
    /// @brief 给角色移动加上抛出的根运动；客户端要等本机门叶也开好才开始
    void ApplyEjectionMotion();
    /// @brief 去掉抛出根运动，恢复原来的转向设置
    void RemoveEjectionMotion();
    /// @brief 抛出结束或被打断时停下移动，切到下落阶段
    void BeginEncounterFall(FName Reason);
    /// @brief 切到等人扶；二狗被截在店里时门先开着，否则关门
    void ConfirmEncounterLanding();
    /// @brief 初遇没开始成功时关门、清掉阻挡状态，回到等待开始
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
    bool bKeepDoorOpenForRescue=false;
    double LastNearbyEncounterWitnessAt=0;
    double NextEncounterCheck=0;
};
