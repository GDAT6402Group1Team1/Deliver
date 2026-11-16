#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Combat/Types/DeliveryImpactReceiver.h"
#include "Vehicle/Motorbike/DeliveryHornReceiver.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "Engine/DataTable.h"
#include "AbilitySystemInterface.h"
#include "GameplayAbilitySpecHandle.h"
#include "Combat/Types/DeliveryNpcStrikeExecutor.h"
#include "AI/Characters/DeliveryNpcProfile.h"
#include "DeliveryNpcCharacter.generated.h"
class UDeliveryNpcAgentComponent;
class UDeliveryNpcCombatComponent;
class UDeliveryNpcPresentationComponent;
class UDeliveryVehicleHoldComponent;
class UDeliveryInteractableComponent;
class UBehaviorTree;
class UCapsuleComponent;
class UDeliveryNpcLookComponent;
class APlayerState;
class UAbilitySystemComponent;
class UGA_NpcStrike;

/**
 * @brief 街坊 NPC 本体：接收游戏里发生的事，转给对应组件，再按决定好的动作去做
 *
 * 交谈：玩家或附近 NPC 说话时从 HearSpeech 开始读，记成事件交给 Agent 决策组件
 * 执行：Agent 采纳动作后，行为树调 ExecuteAction，这里按动作类型去走、去看、去打
 * 战斗：要出拳时从 RequestStrike 开始读，经 GAS 的 GA_NpcStrike 交给战斗组件挥拳
 * 受击：被打、被撞从 ReceiveMeleeHitFrom、ReceiveImpactFrom 开始，交给战斗组件判定倒不倒
 * 决策：Tick 里决定什么时候再问一次模型，失败了按次数重试
 */
UCLASS()
class DELIVERY_API ADeliveryNpcCharacter : public ACharacter, public IDeliveryImpactReceiver, public IDeliveryHornReceiver, public IAbilitySystemInterface, public IDeliveryNpcStrikeExecutor
{
    GENERATED_BODY()
    friend class UDeliveryNpcAgentComponent;
    friend class FDeliveryNpcConversationContextTest;
    friend class FDeliveryConversationFeedbackTest;
public:
    ADeliveryNpcCharacter();

    // ── 战斗 ──
    // RequestStrike 激活 GA_NpcStrike，技能再回调 StartNpcStrike 交给战斗组件挥拳；实现在 DeliveryNpcAbilities.cpp

    /// NPC 自己的技能组件，只用来跑出拳技能
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|技能") TObjectPtr<UAbilitySystemComponent> AbilitySystem;

    /// 出拳用的技能类，在蓝图里填 GA_NpcStrike 或它的子类
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="Delivery|技能") TSubclassOf<UGA_NpcStrike> StrikeAbilityClass;

    /// @brief GAS 通过它找到上面的技能组件
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

    /// @brief 请求朝目标出一拳
    /// @return 技能已激活时返回 true，打没打中看 GetNpcStrikeResult
    bool RequestStrike(AActor* Target,FGuid ActionId);

    /// @brief 交战中的一拳；每拳有自己的 PunchId，对方据此保证同一拳只挨一次
    /// @return 技能已激活时返回 true
    bool RequestFightStrike(AActor* Target,FGuid ActionId /* 交战动作编号 */,FGuid PunchId /* 这一拳的编号 */);

    /// @brief 取消这个动作还没打完的拳
    void CancelStrikeAbility(FGuid ActionId);

    /// @brief GA_NpcStrike 激活后回调，交给战斗组件开始挥拳
    /// @return 战斗组件接下这一拳时返回 true
    virtual bool StartNpcStrike(const UDeliveryNpcStrikeRequest& Request) override;

    /// @brief 技能查这一拳打中没有，转问战斗组件
    /// @return 还没判定或已取消时返回 false
    virtual bool GetNpcStrikeResult(FGuid PunchId,bool& bOutHit) const override;

    /// @brief 技能被取消时回调，让战斗组件停下这一拳
    virtual void CancelNpcStrike(FGuid ActionId /* 动作编号 */,FGuid PunchId /* 这一拳的编号 */) override;

    /// @brief 技能订阅这一拳结束的通知，返回战斗组件的 OnStrikeFinished
    virtual FDeliveryNpcStrikeFinished& GetNpcStrikeFinishedDelegate() override;

    /// @brief 能不能执行规则安排的动作（比如二狗护主）；街坊一律返回 false，二狗子类放开
    virtual bool CanExecuteGameplayRule(EDeliveryNpcAction Action,AActor* Target,int32 RuleRevision) const { return false; }

    /// @brief 规则安排的交战最远追多远；街坊为 0，表示不追
    virtual float GetGameplayRuleFightApproachRange() const { return 0.f; }

    // ── 组件 ──
    // 决策、战斗、表现、视线各一个组件；ImpactSensor 是接受撞击的胶囊，ConversationInteractable 让玩家按键交谈

    /// 接受车撞和拳头扫掠的胶囊，大小跟着角色胶囊走
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UCapsuleComponent> ImpactSensor;

    /// 决策组件：记忆、问模型、检查模型的回答
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcAgentComponent> Agent;

    /// 战斗组件：挥拳、受击、倒地和起身
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcCombatComponent> Combat;

    /// 表现组件：播放台词、声音、表情和受击反应
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcPresentationComponent> Presentation;

    /// 视线组件：头和眼睛看向谁
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcLookComponent> Look;

    /// 玩家走近按键交谈用的交互入口，回调下面的 CanTalk 和 OpenConversation
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryInteractableComponent> ConversationInteractable;

    /// AI 控制器开始控制这位 NPC 后运行的行为树
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UBehaviorTree> BehaviorTree;

    /// 头顶和交谈面板显示的名字
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|居民") FText DisplayName;

    // ── 人物 ──
    // 从 Design/AI 导入的人物表里选一行，ApplyProfile 套用姓名、性格、声音和动作表现

    /// 人物表里的一行，决定姓名、性格、声音和动作表现
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|人物",meta=(RowType="/Script/Delivery.DeliveryNpcProfileRow")) FDataTableRowHandle Profile;

    /// @brief 从人物表套用姓名、性格、声音和动作表现
    /// @return 没选到有效行时返回 false，原设置不变
    UFUNCTION(BlueprintCallable,CallInEditor,Category="Delivery|人物") bool ApplyProfile();

    /// @brief 找某个行为的动作表现，要和当前骨架兼容；没配时返回空，由动画蓝图用默认动作
    const FDeliveryNpcBehaviorPresentation* FindBehaviorPresentation(FGameplayTag Behavior) const;

    // ── 执行 ──
    // Agent 先调用这里的函数，查询某个动作现在能不能做、能走到哪；采纳后由行为树调用下面的 ExecuteAction 执行
    // 这里的查询都不移动 NPC

    /// @brief NPC 此刻能不能做这个动作
    virtual bool CanExecute(EDeliveryNpcAction Action, AActor* Target) const;

    /// @brief 带上请求者再判断一次，比如只有主人能让二狗跑腿；请求者由服务器记录，不信模型给的说话对象
    virtual bool CanExecuteForRequester(EDeliveryNpcAction Action,AActor* Target,APlayerState* Requester) const;

    /// @brief 子类往模型请求里补充自己的内容，二狗用它加主人和安排
    virtual void ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const {}

    /// 走到目标多近算到达
    static constexpr float MoveAcceptanceRadius=140.f;

    /// @brief 是否已走到目标旁边，按导航的到达判断，算上双方胶囊和高度差
    bool HasReachedMoveTarget(AActor* Target) const;

    /// @brief 导航能不能完整走到目标
    bool CanReachTarget(AActor* Target) const;

    /// @brief 找一个踱步的落点
    bool FindPaceDestination(FVector& OutLocation) const;

    /// @brief 找一个往旁边让开的落点
    bool FindSideStepDestination(AActor* Target,FVector& OutLocation) const;

    /// @brief 找一个靠近 Desired、路程不超过 MaxTravel、胶囊站得下的落点
    bool FindNearbyWalkDestination(const FVector& Desired,float MaxTravel,FVector& OutLocation) const;

    /// @brief 现在能不能挪站位：空闲、聊天、等回复时可以，走路和打架时不行
    virtual bool CanAdjustStandingPosition() const;

    /// @brief 离旁边的人太近时，找一个隔开 BodyGap 的站位
    bool FindStandingSpace(AActor* Neighbor,float BodyGap,FVector& OutLocation) const;

    /// @brief 找一个远离威胁、走得到、胶囊过得去的逃跑落点
    bool FindFleeDestination(AActor* Threat,float RemainingTravel,FVector& OutLocation) const;

    // ── 配置 ──
    // 走路、踱步、生气冲过去的速度，以及让开、逃跑的距离

    /// 平常走路的速度
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="30",ClampMax="400",Units="cm/s")) float WalkSpeed=180;

    /// 原地踱步的速度
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="30",ClampMax="180",Units="cm/s")) float PaceSpeed=80;

    /// 生气冲向别人时的速度
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="80",ClampMax="400",Units="cm/s")) float AngryApproachSpeed=240;

    /// 踱步一次走多远
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="40",ClampMax="140",Units="cm")) float PaceDistance=100;

    /// 让路时往旁边让多远
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="80",ClampMax="250",Units="cm")) float SideStepDistance=180;

    /// 逃跑时每段跑多远
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|逃跑",meta=(ClampMin="80",ClampMax="350",Units="cm")) float FleeStepDistance=240;

    /// 离威胁多远算安全
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|逃跑",meta=(ClampMin="200",ClampMax="900",Units="cm")) float FleeSafeDistance=500;

    /// 一次逃跑最多跑多远
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|逃跑",meta=(ClampMin="200",ClampMax="1200",Units="cm")) float FleeTravelLimit=700;

    // ── 执行 ──

    /// @brief 行为树调用，按动作类型开始去做
    /// @return 已开始时返回 true；走没走到、打没打中要等行为树之后检查
    virtual bool ExecuteAction(const FDeliveryNpcPublicState& State);

    /// @brief 停下这个编号的动作；编号对不上的旧通知不影响当前动作
    virtual void StopAction(FGuid ActionId);

    /// @brief 记下正拽着的摩托车
    void SetHeldVehicle(UDeliveryVehicleHoldComponent* Hold);

    /// @brief 正拽着的摩托车，没拽时返回空
    UDeliveryVehicleHoldComponent* GetHeldVehicle() const { return HeldVehicle.Get(); }

    // ── 受击 ──
    // 被撞、被拳头打中、被按喇叭时从这里进；伤害交给战斗组件判定，同时把这件事记进 Agent

    /// @brief 被撞，不知道是谁撞的
    virtual void ReceiveImpact(const FVector& Impulse,const FVector& Point) override;

    /// @brief 被车撞或被推，带上是谁撞的，交给战斗组件处理击退和倒地
    virtual void ReceiveImpactFrom(const FVector& Impulse,const FVector& Point,AActor* Source) override;

    /// @brief 是否倒在地上
    virtual bool IsKnockedDown() const override;

    /// @brief 倒地时还能不能挨打，看战斗组件的 bReceiveHitsWhileDown
    virtual bool CanReceiveHitWhileDown() const override;

    /// @brief 被拳头打中，PunchId 用来保证同一拳只算一次，交给战斗组件判定
    virtual void ReceiveMeleeHitFrom(const FVector& Impulse,const FVector& Point,AActor* Source,FGuid PunchId /* 这一拳的编号，同一拳只算一次 */) override;

    /// @brief 听到喇叭，不知道是谁按的
    virtual void ReceiveHorn(float Multiplier) override;

    /// @brief 听到喇叭，记下哪辆车、谁按的；1.5 秒内多次只记一件事
    virtual void ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* Driver) override;

    // ── 交谈 ──
    // 记下谁说的、说给谁听，调用 Agent 的 BeginListening 和 ObserveEvent，再调用 MarkDecisionNeeded，之后问模型怎么回

    /// @brief 接收现场听到的话；玩家的话已经过交谈组件检查，由服务器调过来
    /// bRequestReply 为 false 时只记住，不接话也不打断正在说的回复
    /// @return 听到了返回 true，这时还没回答
    bool HearSpeech(AActor* Speaker /* 说话的人 */,const FString& Text,FGuid TurnId /* 这句话的编号 */,int32 Depth /* 第几层接话，玩家说的是 0 */,AActor* Addressee=nullptr /* 这句话是对谁说的，空表示对自己说 */,FGuid ReplyToEventId=FGuid() /* 这句话在回答哪件事 */,bool bRequestReply=true /* false 时只记住，不回话 */);

    /// @brief 接收主人开麦说的话：再确认一次说话的是这只二狗的主人，记成连麦而不是现场听见
    bool HearCompanionVoice(class APlayerController* Speaker /* 开麦的玩家 */,const FString& Text,FGuid TurnId /* 这句话的编号 */);

    // 开麦时二狗离得远，也要同步给主人那台客户端
    virtual bool IsNetRelevantFor(const AActor* RealViewer,const AActor* ViewTarget,const FVector& SrcLocation) const override;

    /// @brief 当前是第几层接话，玩家开口算第 0 层，NPC 每接一次加一
    int32 GetConversationDepth() const { return ConversationDepth; }

    // ── 决策 ──
    // Tick 里看有没有新事要问模型，到时间就交给 Agent 的 RequestDecision；失败按次数推迟重试

    /// @brief 让下一次请求在 0.25 秒内发出，NPC 之间接话时用
    void WakeDecision();

    /// @brief 上次请求失败后是否已安排重试
    bool IsDecisionRetryScheduled() const { return bDecisionRetryScheduled; }

    /// @brief 把一条见闻写成 Agent 的事件，比如挨了一拳、看见有人冲过来
    void ObserveFact(FName Kind,AActor* Subject,const FString& Text,EDeliveryNpcEvidenceSource Source);
protected:
    // ── 执行 ──

    /// @brief 踱步一次最多走多远，子类可以收紧，街坊不限制
    virtual float GetPaceTravelBudget() const { return TNumericLimits<float>::Max(); }

    // ── 生命周期 ──
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float Dt) override;
private:
    // ── 人物 ──
    // 各端各自从人物表加载，网络只同步动作编号和时间

    /// 每个行为对应的动作表现，ApplyProfile 时填好
    UPROPERTY(Transient) TMap<FGameplayTag,FDeliveryNpcBehaviorPresentation> BehaviorPresentation;

    // ── 交谈 ──
    // HearSpeech 和 HearCompanionVoice 都调用这里：确认说话人还在、记成事件、开始倾听，需要回话时调用 MarkDecisionNeeded

    /// 确认说话人还在、记成事件、开始倾听；bCompanionVoice 表示主人开麦
    bool HearSpeechInternal(AActor* Speaker,const FString& Text,FGuid TurnId /* 这句话的编号 */,int32 Depth /* 第几层接话 */,AActor* Addressee /* 对谁说的 */,
        FGuid ReplyToEventId /* 在回答哪件事 */,bool bRequestReply /* false 时只记住，不回话 */,bool bCompanionVoice /* true 表示主人开麦说的 */);

    // ── 战斗 ──
    // 找到并激活 GA_NpcStrike，fight 表示交战中的连拳

    /// 激活出拳技能，bFight 表示交战中的连拳
    bool RequestStrikeAbility(AActor* Target,FGuid ActionId /* 动作编号 */,FGuid PunchId /* 这一拳的编号 */,bool bFight /* true 表示交战中的连拳 */);

    /// 授予后的出拳技能句柄
    FGameplayAbilitySpecHandle StrikeAbilityHandle;

    /// 执行动作前的走路速度，动作结束时还原；-1 表示没有改过
    float PreviousActionWalkSpeed=-1;

    // ── 决策 ──

    /// 标记有新事要问模型，Tick 里到点就发
    void MarkDecisionNeeded();

    /// 网关确认请求已排上队时调用，把 bDecisionNeeded 设为 false
    void ConfirmDecisionRequestQueued(uint64 WakeSerial /* 发请求时的唤醒次数，之后又来了新事就不清标记 */);

    /// 清掉所有调度状态，比如倒地或二狗换主人时
    void ResetDecisionScheduling();

    /// 一次请求结束：失败时最多重试 2 次，成功时重试次数清零
    UFUNCTION() void OnDecisionFinished(FName Reason);

    /// 连续失败的次数，最多重试 2 次
    int32 DecisionRetryCount=0;

    /// 是否已安排重试
    bool bDecisionRetryScheduled=false;

    // ── 交谈 ──
    // 玩家按键交谈时由 ConversationInteractable 回调

    /// 玩家现在能不能找这位 NPC 交谈，bCheckAvailability 时还要看 NPC 有没有空
    bool CanTalk(APawn* Interactor,bool bCheckAvailability /* true 时还要看 NPC 有没有空 */);

    /// 服务器：玩家按键交谈时，确认能交谈后让玩家打开面板
    bool OpenConversation(APawn* Interactor);

    // ── 受击 ──
    // 正拽着的车，和连续被按喇叭的记录

    /// 正拽着的摩托车
    TWeakObjectPtr<UDeliveryVehicleHoldComponent> HeldVehicle;

    /// 上次听到喇叭的时间
    double LastHorn = -100;

    /// 最近连续听到喇叭的次数，隔久了清零
    int32 HornPressure = 0;

    /// 上次把喇叭记成事件的时间
    double LastHornObservation = -100;

    // ── 决策 ──
    // 什么时候再问模型、失败了重试几次

    /// 下一次请求的最早时间，发出后推迟 4 秒
    double NextDecision = 0;

    /// 当前接话层数，见 GetConversationDepth
    int32 ConversationDepth = 0;

    /// 正在执行的动作编号
    FGuid ExecutingAction;

    /// 最近结束的动作编号；同一个动作的结束通知来两次时，第二次不再执行收拾工作
    FGuid LastFinishedAction;

    // 网关回调期间又来的新事，留给下一次请求
    uint64 DecisionWakeSerial=0;

    /// 有新事要问模型
    bool bDecisionNeeded=true;
};
