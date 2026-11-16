#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Combat/Types/DeliveryNpcStrikeExecutor.h"
#include "DeliveryNpcCombatComponent.generated.h"

class USkeletalMeshComponent;
class APawn;
class AAIController;

/// @brief 有人正在扶 NPC 起来时每帧问一次：这个人还能不能继续扶；二狗用它检查扶的人是不是主人、离得够不够近
DECLARE_DELEGATE_RetVal_TwoParams(bool,FDeliveryNpcCanContinueRecovery,APawn* /* 扶的人 */,FGuid /* 这次倒地的编号 */);

/// @brief NPC 倒地或站起来时通知
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryNpcDownChanged,FGuid /* 这次倒地的编号 */);

/// @brief NPC 完全站起来时通知
DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliveryNpcRecoveryFinished,FGuid /* 这次倒地的编号 */,APawn* /* 扶起它的人，自己起来时为空 */);

/**
 * @brief NPC 的拳脚：挥拳、判定打没打中、挨打、倒地和起身，都在服务器算
 *
 * 出拳：GA_NpcStrike 经角色调 StartStrike，OnStrikePoseUpdated 每帧沿拳头路径扫，FinishStrike 判定后发 OnStrikeFinished
 * 交战：行为树先 BeginFight 记下对手，再反复 StartFightStrike 连拳，GetFightStopReason 说该停时 EndFight
 * 受击：被玩家拳头打中从 ReceiveMeleeImpact 开始，被撞从 ReceiveImpact 开始，够重就倒地
 * 起身：倒地后到时间自己起来，或有人长按扶起从 StartAssistedRecovery 开始
 * 倒地、起身和出拳时间同步给客户端，由表现组件播动画；同一拳只算一次命中
 */
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcCombatComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcLookTest;
    friend class FDeliveryCompanionRulesTest;
    friend class FDeliveryCompanionErrandTest;
public:
    UDeliveryNpcCombatComponent();

    // ── 出拳 ──
    // 先用 CanStrike / CanApproachStrike 判断能不能打，StartStrike 开始挥拳，结果用 GetStrikeResult 查，结束时发 OnStrikeFinished

    /// @brief 现在就能打到：离得够近、中间没挡、自己没倒地；冷却在 StartStrike 里再查
    bool CanStrike(AActor* Target) const;

    /// @brief 走到离目标多近停下来出拳，按双方身体宽度算，只留一小段空隙
    float GetStrikeApproachDistance(const AActor* Target) const;

    /// @brief 能不能走过去打：近身直接算能，远的要导航能完整走到，才列进模型的候选
    bool CanApproachStrike(AActor* Target) const;

    /// @brief 走过去的途中目标还有效、冷却已过；路能不能走由导航任务自己查
    bool CanContinueStrikeApproach(AActor* Target) const;

    /// @brief 开始挥一拳
    /// @return 开始挥拳时返回 true；只有拳头在接触时间段里真的扫到目标才算打中
    bool StartStrike(AActor* Target,FGuid ActionId);

    // ── 交战 ──
    // 模型选对手，BeginFight 记下来；行为树负责走近和连拳，每拳都走 StartFightStrike；GetFightStopReason 返回原因时 EndFight
    // 二狗护主这类规则交战按拳数结束，模型交战按时长结束

    /// @brief 现在能不能开始和这个人打
    bool CanBeginFight(AActor* Target,float AuthorizedApproachRange=0.f) const;

    /// @brief 这个人能不能列进模型的交战候选：身体能出拳、在追击范围内、没被挡住
    bool CanChooseFightTarget(AActor* Target,float AuthorizedApproachRange=0.f) const;

    /// @brief 开始一场交战，记下对手、起点和持续时间
    /// @return 开始时返回 true；bModelDecision 表示模型选的，规则安排的为 false
    bool BeginFight(AActor* Target,FGuid ActionId /* 交战这个动作的编号 */,float Duration /* 最多打几秒 */,bool bModelDecision=false /* true 表示模型选的，false 表示规则安排的 */,float AuthorizedApproachRange=0.f /* 规则交战另给的追击距离，0 用默认 */);

    /// @brief 模型决定接着打同一个人：只延长结束时间，当前这拳、起点和命中数都保留
    bool ContinueFight(FGuid ActionId,float Duration);

    /// @brief 交战中挥一拳，PunchId 保证每拳只算一次
    bool StartFightStrike(AActor* Target,FGuid ActionId /* 交战动作编号 */,FGuid PunchId /* 这一拳的编号 */);

    /// @brief 交战该不该停
    /// @return 该停时返回原因（倒地、对手没了、时间到、拳数够了等），还能打返回 None；收拳和冷却由行为树等
    FName GetFightStopReason(FGuid ActionId) const;

    /// @brief 冷却已过，可以挥下一拳
    bool IsReadyToStrike() const;

    /// @brief 结束交战，bApplyCooldown 时要歇一会儿才能再开打
    void EndFight(FGuid ActionId,bool bApplyCooldown=true /* true 时要歇一会儿才能再开打 */);

    /// @brief 用一句话描述这场交战的结果，写进记忆给模型看
    FString DescribeFight(FGuid ActionId,FName Reason) const;

    /// @brief 描述自己此刻的状况：刚挨了几拳、是否倒地、打出几拳；模型据此决定继续、逃跑还是停手
    FString DescribeCombatState() const;

    /// @brief 这场打出了几拳
    int32 GetFightPunchCount() const { return FightPunches; }

    /// @brief 这场打中了几拳
    int32 GetFightHitCount() const { return FightHits; }

    /// @brief 规则交战最多打几拳，限制在 1 到 5
    int32 GetFightPunchLimit() const { return FMath::Clamp(FightPunchLimit,1,5); }

    // ── 出拳 ──

    /// @brief 查这一拳打中没有
    /// @return 已判定时返回 true，结果写在 bOutHit；还没判定或已取消时返回 false
    bool GetStrikeResult(FGuid ActionId,bool& bOutHit) const;

    /// @brief 这一拳开始的服务器时间，客户端据此对齐出拳动画；还在走近时返回负数
    double GetStrikeStartTime(FGuid ActionId) const;

    /// @brief 当前正在播的这一拳的编号
    FGuid GetCurrentPunchId() const { return StrikeAnimationId; }

    // ── 受击 ──

    /// @brief 最近一次后仰的开始时间
    double GetHitReactionStartTime() const { return HitReactionStartedAt; }

    /// @brief 是否正在挨打后仰，0.55 秒内
    bool IsReactingToHit() const;

    /// 挨打后仰持续多久
    static constexpr float HitReactionDuration=.55f;

    // ── 起身 ──

    /// @brief 倒地开始的服务器时间，没倒地时返回 -1；迟到的客户端据此接上当前姿势
    double GetDownStartTime() const { return bDown?DownStartedAt:-1; }

    /// @brief 起身开始的服务器时间，没在起身时返回 -1
    double GetRecoveryStartTime() const { return bRecovering?RecoveryStartedAt:-1; }

    // ── 出拳 ──

    /// 一拳总共多长
    static constexpr float StrikeDuration=1.2f;

    /// 挥拳前的蓄力时间
    static constexpr float StrikeWindup=.35f;

    /// @brief 取消当前这拳，还没发生的命中不再判定
    void CancelStrike();

    /// @brief 只在动作编号和拳编号都对上时取消
    void CancelStrike(FGuid ActionId /* 动作编号 */,FGuid PunchId /* 这一拳的编号，两个都对上才取消 */);

    /// 一拳判定完时通知，GA_NpcStrike 据此结束技能
    FDeliveryNpcStrikeFinished OnStrikeFinished;

    // ── 受击 ──
    // 被拳头打中从 ReceiveMeleeImpact 进，被撞从 ReceiveImpact 进；够重就倒地，同时告诉角色记下这件事，需要时立即还手

    /// @brief 被拳头打中，PunchId 相同的只算一次
    void ReceiveMeleeImpact(const FVector& Impulse,const FVector& Point,AActor* Source,FGuid PunchId /* 这一拳的编号，同一拳只算一次 */);

    // ── 配置 ──

    /// 打中玩家扣多少血
    UPROPERTY(EditAnywhere,Category="Delivery|攻击效果",meta=(ClampMin="0")) float OutgoingPlayerDamage=10;

    /// 打中玩家时推多大力
    UPROPERTY(EditAnywhere,Category="Delivery|攻击效果",meta=(ClampMin="0")) float OutgoingPlayerImpulse=900;

    /// 打中路人等可撞对象时推多大力
    UPROPERTY(EditAnywhere,Category="Delivery|攻击效果",meta=(ClampMin="0")) float OutgoingImpactImpulse=35000;

    /// 只轻拍：打中仍算数、对方会后仰，但不扣血、不推人、不累计击倒
    UPROPERTY(EditAnywhere,Category="Delivery|攻击效果") bool bHarmlessStrikes=false;

    /// 挨一拳就倒
    UPROPERTY(EditAnywhere,Category="Delivery|受击") bool bKnockDownOnAnyPunch=false;

    /// 倒地时还能挨打
    UPROPERTY(EditAnywhere,Category="Delivery|受击") bool bReceiveHitsWhileDown=false;

    /// 挨打后不等模型，后仰完或起身后直接追打动手的人；之后继续、逃跑或停手仍由模型选
    UPROPERTY(EditAnywhere,Category="Delivery|受击") bool bRetaliateWhenHit=true;

    /// @brief 还手时清掉交战冷却，可以马上开打
    void AllowImmediateFight() { NextFightAt=0; }

    /// 倒地时又挨打，多躺几秒
    UPROPERTY(EditAnywhere,Category="Delivery|受击",meta=(ClampMin="0",ClampMax="10",Units="s")) float DownHitExtensionSeconds=4;

    /// 被打倒时水平飞出的速度
    UPROPERTY(EditAnywhere,Category="Delivery|受击",meta=(ClampMin="0",ClampMax="2000",Units="cm/s")) float KnockdownHorizontalSpeed=450;

    /// 被打倒时往上弹的速度
    UPROPERTY(EditAnywhere,Category="Delivery|受击",meta=(ClampMin="0",ClampMax="2000",Units="cm/s")) float KnockdownUpwardSpeed=100;

    // ── 受击 ──

    /// @brief 服务器：被车撞或被推时立刻倒地，不等模型
    /// 停下正在挥的拳和当前安排、松开拽着的车，用 LaunchCharacter 把 NPC 推出去，到 RecoverySeconds 后自己起来
    void ReceiveImpact(const FVector& Impulse,AActor* Source);

    /// @brief 撤掉挨打时转头看向打人者的朝向；只撤这一层，模型选的朝向不动
    void ResetHitFacing();

    // ── 起身 ──
    // 倒地后到时间自己起来；有人扶从 StartAssistedRecovery 开始，扶的过程每帧问 CanContinueAssistedRecovery
    // 起身完成发 OnRecoveryFinished；主动躺下用 SetDeliberateProne，起来用 StartDeliberateRecovery

    /// @brief 主动躺下或取消躺下，不是被打倒
    bool SetDeliberateProne(bool bProne /* true 躺下，false 取消躺下 */);

    /// @brief 主动躺下时能不能起来
    bool CanStartDeliberateRecovery() const;

    /// @brief 从主动躺下开始起身，已经在起身时接着起
    bool StartDeliberateRecovery();

    /// 起身动作多长
    static constexpr float RecoveryMotionSeconds=1.6f;

    /// @brief 是否倒在地上
    UFUNCTION(BlueprintPure) bool IsDown() const { return bDown; }

    /// @brief 是主动躺下的；主动躺下还能说话，被打倒时立刻停止说话
    bool IsDeliberatelyProne() const { return bDown && bDeliberate; }

    /// @brief 是否正在起身
    UFUNCTION(BlueprintPure) bool IsRecovering() const { return bRecovering; }

    /// @brief 这次倒地的编号，每次被打倒换一个；倒地时又挨打编号不变，但正在扶的会被打断
    FGuid GetDownEpisodeId() const { return DownEpisodeId; }

    /// @brief 现在能不能扶：倒着、不在起身、不是主动躺下
    bool CanStartAssistedRecovery() const;

    /// @brief 有人开始扶，倒地编号要对上
    /// @return 开始起身时返回 true，真正站起来要等 OnRecoveryFinished
    bool StartAssistedRecovery(FGuid ExpectedDownEpisode,APawn* Helper);

    /// @brief 不扶了，回到原来的自然起身时间
    void CancelAssistedRecovery(FGuid ExpectedDownEpisode);

    /// @brief 这次起身是不是这个人扶起来的
    bool WasRecoveryAssistedBy(FGuid Episode,APawn* Helper) const;

    /// 扶的过程中由外部检查扶的人还合不合格（权限、距离、遮挡）；这里只管身体和起身时间
    FDeliveryNpcCanContinueRecovery CanContinueAssistedRecovery;

    /// 倒地或站起时通知
    FDeliveryNpcDownChanged OnDownChanged;

    /// 起身完成时通知
    FDeliveryNpcRecoveryFinished OnRecoveryFinished;

    // ── 出拳 ──

    /// @brief 是否正在挥拳
    UFUNCTION(BlueprintPure) bool IsStriking() const { return AttackId.IsValid(); }

    // ── 配置 ──

    /// 站着不动能打到多远
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="50",ClampMax="250",Units="cm")) float StrikeRange=160;

    /// 出拳的手骨名字
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触") FName StrikeHandBone=TEXT("LeftHand");

    /// 检测球相对手骨的偏移，按模型握拳的位置调，让球贴着拳头；不要用它加长攻击距离
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(Units="cm")) FVector StrikeHandOffset=FVector::ZeroVector;

    /// 拳头检测球的半径
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(ClampMin="2",ClampMax="12",Units="cm")) float StrikeHandRadius=8;

    /// 走近时和对方身体留多少空隙
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(ClampMin="0",ClampMax="12",Units="cm")) float StrikeBodyGap=4;

    /// 挥拳后多久开始算接触
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(ClampMin="0",ClampMax="0.5",Units="s")) float StrikeContactStart=.12f;

    /// 挥拳后多久不再算接触
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(ClampMin="0.15",ClampMax="0.8",Units="s")) float StrikeContactEnd=.4f;

    /// 最远走多远过去打
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="160",ClampMax="1200",Units="cm")) float StrikeApproachRange=600;

    /// 被打倒后躺多久自己起来
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="1",ClampMax="10",Units="s")) float RecoverySeconds=4;

    /// 规则交战最多打几拳
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="1",ClampMax="5")) int32 FightPunchLimit=3;

    /// 模型交战连挥几拳后歇多久；规则交战按拳数结束，不用这个
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="0",ClampMax="1.5",Units="s")) float FightBurstPauseSeconds=.75f;

    /// 交战中离起点最远追多远
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="160",ClampMax="1200",Units="cm")) float FightTravelRange=600;

    /// 一场交战结束后歇多久才能再开打
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="1",ClampMax="15",Units="s")) float FightCooldown=4;

    /// 交战中挨几记轻拳会倒下，普通重击照原来的规则直接倒
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="2",ClampMax="8")) int32 LightHitsToKnockDown=4;

    /// 多久没挨轻拳就清零累计
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="2",ClampMax="20",Units="s")) float LightHitResetSeconds=10;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    // ── 出拳 ──
    // StartStrikeInternal 记下目标和时间 → OnStrikePoseUpdated 每帧在接触时间段里沿拳头路径扫球、查遮挡 → FinishStrike 判定、通知、扣血

    /// 距离、遮挡、目标状态都满足
    bool CanStrikeTarget(AActor* Target,float MaxDistance) const;

    /// 记下目标、时间和编号，挂上拳头扫掠；bLightHit 表示交战中的轻拳
    bool StartStrikeInternal(AActor* Target,FGuid PunchId /* 这一拳的编号 */,FGuid ActionId /* 所属动作的编号 */,bool bLightHit /* true 表示交战中的轻拳 */);

    /// 身体有出拳用的骨骼和动画
    bool HasStrikePose() const;

    /// 每次骨骼更新后：在接触时间段里沿拳头这一帧走过的路径扫球，查遮挡，碰到目标就 FinishStrike
    void OnStrikePoseUpdated();

    /// 判定这一拳：记日志、打中就扣血推人、发 OnStrikeFinished
    void FinishStrike(bool bHit /* 打中了没有 */,const FVector& ImpactPoint);

    /// 挥拳时为了每帧更新骨骼改过网格的更新设置，这里还原
    void RestoreMeshTickSettings();

    // ── 受击 ──
    // ReceiveFightHit 累计轻拳 → 打断当前动作和说话 → 播受击反应 → 记事件 → 需要时还手；同时让 NPC 转头看打人者

    /// 交战中挨了一拳：够数就倒，否则后仰
    void ReceiveFightHit(AActor* Source,FGuid PunchId /* 这一拳的编号 */,bool bHarmless=false /* true 表示只是轻拍，不累计倒地 */);

    /// 记下这一拳，已经记过的返回 false
    bool RememberPunch(FGuid PunchId);

    /// 挨打时让 AI 控制器转头看向打人者
    void RequestHitFacing(AActor* Source,FGuid HitId);

    /// 到时间就撤掉转头
    void UpdateHitFacing(double Now);

    /// 倒地时记下打人者，起身后转向他
    void RememberRecoveryFacing(AActor* Source);

    /// 起身后转向打人者，倒地编号要对上
    void TryFaceSourceAfterRecovery(FGuid Episode);

    /// 开始起身动作
    void BeginRecovery(double Now);

    // ── 受击 ──
    // 挨打转头的状态

    /// 设置转头的 AI 控制器
    TWeakObjectPtr<AAIController> HitFacingController;

    /// 转头看向的打人者
    TWeakObjectPtr<AActor> HitFacingSource;

    /// 转头前正在看的人，撤掉时还原
    TWeakObjectPtr<AActor> HitFacingListener;

    /// 这次转头对应的那一拳
    FGuid HitFacingId;

    /// 转头时正在执行的动作
    FGuid HitFacingActionId;

    /// 什么时候撤掉转头
    double HitFacingEndsAt=0;

    /// 转头已经生效
    bool bHitFacingApplied=false;

    /// 起身后要转向的人
    TWeakObjectPtr<AActor> RecoveryFacingSource;

    /// 这次是谁把我打倒的，起身后凭这条受击记录还手
    TWeakObjectPtr<AActor> KnockdownAttacker;

    /// 起身转向对应哪次倒地
    FGuid RecoveryFacingEpisode;

    /// 正在撤转头，防止回调里再撤一次
    bool bEndingHitFacing=false;

    // ── 起身 ──

    /// 倒地时又挨打，延长躺着的时间
    void ExtendDownTime(AActor* Source);

    // ── 同步给客户端的身体状态 ──
    // 客户端按这些时间对齐出拳、倒地、起身和后仰动画

    /// 是否倒地
    UPROPERTY(ReplicatedUsing=OnRep_Down) bool bDown=false;

    /// 是不是主动躺下
    UPROPERTY(Replicated) bool bDeliberate=false;

    /// 是否正在起身
    UPROPERTY(Replicated) bool bRecovering=false;

    /// 客户端收到倒地变化，通知角色和表现
    UFUNCTION() void OnRep_Down();

    /// 服务器正在判定的这一拳
    FGuid AttackId;

    /// 客户端要播的这一拳
    UPROPERTY(Replicated) FGuid StrikeAnimationId;

    /// 这一拳属于哪个动作
    UPROPERTY(Replicated) FGuid StrikeActionId;

    /// 这一拳的开始时间
    UPROPERTY(Replicated) double StrikeStartedAt=-1;

    /// 倒地的开始时间
    UPROPERTY(Replicated) double DownStartedAt=-1;

    /// 起身的开始时间
    UPROPERTY(Replicated) double RecoveryStartedAt=-1;

    /// 这次倒地的编号
    UPROPERTY(Replicated) FGuid DownEpisodeId;

    /// 后仰的开始时间
    UPROPERTY(Replicated) double HitReactionStartedAt=-1;

    // ── 出拳 ──
    // 服务器这一拳的进度

    /// 最近判定完的一拳
    FGuid FinishedStrikeId;

    /// 最近判定完的那拳打中没有
    bool bFinishedStrikeHit=false;

    /// 这一拳的目标
    TWeakObjectPtr<AActor> AttackTarget;

    /// 用来读拳头位置的网格
    TWeakObjectPtr<USkeletalMeshComponent> StrikeMesh;

    /// 骨骼更新回调的句柄
    FDelegateHandle StrikePoseHandle;

    /// 上一帧拳头的位置，和这一帧连成扫掠路径
    FVector PreviousHandPosition=FVector::ZeroVector;

    /// 上一帧自己的位置
    FVector PreviousOwnerPosition=FVector::ZeroVector;

    /// 上一帧拳头位置的时间
    double PreviousHandTime=-1;

    /// 改之前网格的骨骼更新设置
    uint8 PreviousMeshVisibility=0;

    /// 改之前网格的降频设置
    bool bPreviousUpdateRateOptimization=false;

    /// 是否改过网格设置
    bool bOverrideMeshTick=false;

    // ── 起身 ──

    /// 什么时候开始起身
    double RecoverAt=0;

    /// 正在被人扶
    bool bAssistedRecovery=false;

    /// 没人扶时原本的起身时间，取消扶起后回到它
    double UnassistedRecoverAt=0;

    /// 扶的人
    TWeakObjectPtr<APawn> RecoveryHelper;

    /// 最近完成起身的倒地编号
    FGuid CompletedRecoveryEpisode;

    /// 最近那次是谁扶起来的
    TWeakObjectPtr<APawn> CompletedRecoveryHelper;

    // ── 交战 ──

    /// 下一拳最早什么时候能挥
    double NextStrikeAt=0;

    /// 当前这拳是交战中的轻拳
    bool bCurrentStrikeIsLight=false;

    /// 这场交战的动作编号
    FGuid FightActionId;

    /// 对手
    TWeakObjectPtr<AActor> FightTarget;

    /// 开打时站的位置，离太远就停
    FVector FightOrigin=FVector::ZeroVector;

    /// 规则交战另给的追击距离
    float FightApproachOverride=0.f;

    /// 交战结束时间
    double FightEndsAt=0;

    /// 下一场最早什么时候能开打
    double NextFightAt=0;

    /// 这场打出的拳数
    int32 FightPunches=0;

    /// 这场打中的拳数
    int32 FightHits=0;

    /// 是模型选的交战
    bool bModelFight=false;

    /// 这一轮连拳打了几拳，够数歇一下
    int32 FightBurstPunches=0;

    /// 累计挨的轻拳
    int32 ReceivedLightHits=0;

    /// 上次挨轻拳的时间
    double LastLightHitAt=-100;

    /// 最近挨过的拳编号，同一拳只算一次
    TArray<FGuid> RecentLightHits;
};
