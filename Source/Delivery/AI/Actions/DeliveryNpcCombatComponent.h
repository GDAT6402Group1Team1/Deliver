#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Combat/DeliveryNpcStrikeExecutor.h"
#include "DeliveryNpcCombatComponent.generated.h"

class USkeletalMeshComponent;
class APawn;
class AAIController;

DECLARE_DELEGATE_RetVal_TwoParams(bool,FDeliveryNpcCanContinueRecovery,APawn*,FGuid);
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryNpcDownChanged,FGuid);
DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliveryNpcRecoveryFinished,FGuid,APawn*);

// 在服务器处理 NPC 挥拳、受击和起身，同一拳只判定一次命中
// 倒地和起身状态同步给客户端，由表现组件播放相应动画
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcCombatComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcLookTest;
    friend class FDeliveryCompanionRulesTest;
    friend class FDeliveryCompanionErrandTest;
public:
    UDeliveryNpcCombatComponent();
    // 检查距离、遮挡和身体状态，攻击冷却在 StartStrike 中检查
    bool CanStrike(AActor* Target) const;
    // 导航靠近到双方身体之间只剩小段间隙，命中仍由实际拳头接触决定
    float GetStrikeApproachDistance(const AActor* Target) const;
    // 近身可直接挥拳，较远的目标必须有完整导航路径才提供给模型
    bool CanApproachStrike(AActor* Target) const;
    // 接近途中检查目标和攻击冷却，实际路径由正在运行的导航任务检查
    bool CanContinueStrikeApproach(AActor* Target) const;
    // 返回 true 表示开始挥拳，只有有效窗口内的拳头扫掠碰到目标才算命中
    bool StartStrike(AActor* Target,FGuid ActionId);
    // 模型选择目标，行为树负责接近和出拳；护主规则仍使用原来的短轮次数限制
    bool CanBeginFight(AActor* Target,float AuthorizedApproachRange=0.f) const;
    bool CanChooseFightTarget(AActor* Target,float AuthorizedApproachRange=0.f) const;
    bool BeginFight(AActor* Target,FGuid ActionId,float Duration,bool bModelDecision=false,float AuthorizedApproachRange=0.f);
    // 同目标的新决定只更新结束时间，保留当前拳、起点和累计命中
    bool ContinueFight(FGuid ActionId,float Duration);
    bool StartFightStrike(AActor* Target,FGuid ActionId,FGuid PunchId);
    // 返回停止原因，空值表示还能继续；收拳与冷却由行为树等待
    FName GetFightStopReason(FGuid ActionId) const;
    bool IsReadyToStrike() const;
    void EndFight(FGuid ActionId,bool bApplyCooldown=true);
    FString DescribeFight(FGuid ActionId,FName Reason) const;
    // 只介绍自己实际经历的受击与出拳，供模型判断继续交战、逃跑或停手
    FString DescribeCombatState() const;
    int32 GetFightPunchCount() const { return FightPunches; }
    int32 GetFightHitCount() const { return FightHits; }
    int32 GetFightPunchLimit() const { return FMath::Clamp(FightPunchLimit,1,5); }
    // 只查询这一次挥拳的判定结果，挥拳尚未判定或已取消时返回 false
    bool GetStrikeResult(FGuid ActionId,bool& bOutHit) const;
    // 客户端按服务器时间播放当前这一拳，接近目标时没有出拳时间
    double GetStrikeStartTime(FGuid ActionId) const;
    FGuid GetCurrentPunchId() const { return StrikeAnimationId; }
    double GetHitReactionStartTime() const { return HitReactionStartedAt; }
    bool IsReactingToHit() const;
    static constexpr float HitReactionDuration=.55f;
    // 倒地与起身也按服务器开始时间显示，迟到的客户端接上当前姿势
    double GetDownStartTime() const { return bDown?DownStartedAt:-1; }
    double GetRecoveryStartTime() const { return bRecovering?RecoveryStartedAt:-1; }
    static constexpr float StrikeDuration=1.2f;
    static constexpr float StrikeWindup=.35f;
    // 清除攻击目标和编号，取消后不再判定尚未发生的命中
    void CancelStrike();
    void CancelStrike(FGuid ActionId,FGuid PunchId);
    FDeliveryNpcStrikeFinished OnStrikeFinished;
    void ReceiveMeleeImpact(const FVector& Impulse,const FVector& Point,AActor* Source,FGuid PunchId);
    UPROPERTY(EditAnywhere,Category="Delivery|攻击效果",meta=(ClampMin="0")) float OutgoingPlayerDamage=10;
    UPROPERTY(EditAnywhere,Category="Delivery|攻击效果",meta=(ClampMin="0")) float OutgoingPlayerImpulse=900;
    UPROPERTY(EditAnywhere,Category="Delivery|攻击效果",meta=(ClampMin="0")) float OutgoingImpactImpulse=35000;
    // 轻拍保留接触事实与短反应，不扣玩家血、不推动身体、不累计居民击倒
    UPROPERTY(EditAnywhere,Category="Delivery|攻击效果") bool bHarmlessStrikes=false;
    UPROPERTY(EditAnywhere,Category="Delivery|受击") bool bKnockDownOnAnyPunch=false;
    UPROPERTY(EditAnywhere,Category="Delivery|受击") bool bReceiveHitsWhileDown=false;
    // 挨打后不等模型回复，后仰结束或起身后直接追打动手的人；继续、逃跑或停手仍由模型在交战中选择
    UPROPERTY(EditAnywhere,Category="Delivery|受击") bool bRetaliateWhenHit=true;
    // 还手可以立即开始，不受上一轮交战结束后的冷却限制
    void AllowImmediateFight() { NextFightAt=0; }
    UPROPERTY(EditAnywhere,Category="Delivery|受击",meta=(ClampMin="0",ClampMax="10",Units="s")) float DownHitExtensionSeconds=4;
    UPROPERTY(EditAnywhere,Category="Delivery|受击",meta=(ClampMin="0",ClampMax="2000",Units="cm/s")) float KnockdownHorizontalSpeed=450;
    UPROPERTY(EditAnywhere,Category="Delivery|受击",meta=(ClampMin="0",ClampMax="2000",Units="cm/s")) float KnockdownUpwardSpeed=100;

    // 在服务器处理击退和倒地，不等待模型回复
    void ReceiveImpact(const FVector& Impulse,AActor* Source);
    // 控制器退出时只清理本组件的受击焦点，不取消模型选择或其他朝向层
    void ResetHitFacing();
    bool SetDeliberateProne(bool bProne);
    bool CanStartDeliberateRecovery() const;
    // 开始起身，或接续已经开始的自主起身
    bool StartDeliberateRecovery();
    static constexpr float RecoveryMotionSeconds=1.6f;
    UFUNCTION(BlueprintPure) bool IsDown() const { return bDown; }
    // 主动躺下保留说话能力，被击倒时仍立即停止对白
    bool IsDeliberatelyProne() const { return bDown && bDeliberate; }
    UFUNCTION(BlueprintPure) bool IsRecovering() const { return bRecovering; }
    // 每次真实击倒有独立编号，倒地重击仍属于同一轮但会撤销正在进行的扶起
    FGuid GetDownEpisodeId() const { return DownEpisodeId; }
    bool CanStartAssistedRecovery() const;
    // true 只表示开始原起身流程，完成事件才表示已经恢复站立
    bool StartAssistedRecovery(FGuid ExpectedDownEpisode,APawn* Helper);
    void CancelAssistedRecovery(FGuid ExpectedDownEpisode);
    bool WasRecoveryAssistedBy(FGuid Episode,APawn* Helper) const;
    // 接入者复核帮助者权限、距离和遮挡；战斗组件只管理身体状态与起身时序
    FDeliveryNpcCanContinueRecovery CanContinueAssistedRecovery;
    FDeliveryNpcDownChanged OnDownChanged;
    FDeliveryNpcRecoveryFinished OnRecoveryFinished;
    UFUNCTION(BlueprintPure) bool IsStriking() const { return AttackId.IsValid(); }
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="50",ClampMax="250",Units="cm")) float StrikeRange=160;
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触") FName StrikeHandBone=TEXT("LeftHand");
    // 可按模型握拳位置调整，保持检测球贴着拳头，不用偏移延长攻击距离
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(Units="cm")) FVector StrikeHandOffset=FVector::ZeroVector;
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(ClampMin="2",ClampMax="12",Units="cm")) float StrikeHandRadius=8;
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(ClampMin="0",ClampMax="12",Units="cm")) float StrikeBodyGap=4;
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(ClampMin="0",ClampMax="0.5",Units="s")) float StrikeContactStart=.12f;
    UPROPERTY(EditAnywhere,Category="Delivery|拳头接触",meta=(ClampMin="0.15",ClampMax="0.8",Units="s")) float StrikeContactEnd=.4f;
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="160",ClampMax="1200",Units="cm")) float StrikeApproachRange=600;
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="1",ClampMax="10",Units="s")) float RecoverySeconds=4;
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="1",ClampMax="5")) int32 FightPunchLimit=3;
    // 模型交战连挥几拳后短暂收拳，护主规则仍按次数结束短轮动作
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="0",ClampMax="1.5",Units="s")) float FightBurstPauseSeconds=.75f;
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="160",ClampMax="1200",Units="cm")) float FightTravelRange=600;
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="1",ClampMax="15",Units="s")) float FightCooldown=4;
    // 只累计持续交战中的轻拳，普通重击仍走原来的倒地流程
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="2",ClampMax="8")) int32 LightHitsToKnockDown=4;
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="2",ClampMax="20",Units="s")) float LightHitResetSeconds=10;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    bool CanStrikeTarget(AActor* Target,float MaxDistance) const;
    bool StartStrikeInternal(AActor* Target,FGuid PunchId,FGuid ActionId,bool bLightHit);
    bool HasStrikePose() const;
    void OnStrikePoseUpdated();
    void FinishStrike(bool bHit,const FVector& ImpactPoint);
    void RestoreMeshTickSettings();
    void ReceiveFightHit(AActor* Source,FGuid PunchId,bool bHarmless=false);
    bool RememberPunch(FGuid PunchId);
    void RequestHitFacing(AActor* Source,FGuid HitId);
    void UpdateHitFacing(double Now);
    void RememberRecoveryFacing(AActor* Source);
    void TryFaceSourceAfterRecovery(FGuid Episode);
    void BeginRecovery(double Now);
    TWeakObjectPtr<AAIController> HitFacingController;
    TWeakObjectPtr<AActor> HitFacingSource;
    TWeakObjectPtr<AActor> HitFacingListener;
    FGuid HitFacingId;
    FGuid HitFacingActionId;
    double HitFacingEndsAt=0;
    bool bHitFacingApplied=false;
    TWeakObjectPtr<AActor> RecoveryFacingSource;
    // 这轮倒地是谁打的，起身后凭仍有效的受击记录还手
    TWeakObjectPtr<AActor> KnockdownAttacker;
    FGuid RecoveryFacingEpisode;
    bool bEndingHitFacing=false;
    void ExtendDownTime(AActor* Source);
    UPROPERTY(ReplicatedUsing=OnRep_Down) bool bDown=false;
    UPROPERTY(Replicated) bool bDeliberate=false;
    UPROPERTY(Replicated) bool bRecovering=false;
    UFUNCTION() void OnRep_Down();
    FGuid AttackId;
    UPROPERTY(Replicated) FGuid StrikeAnimationId;
    UPROPERTY(Replicated) FGuid StrikeActionId;
    UPROPERTY(Replicated) double StrikeStartedAt=-1;
    UPROPERTY(Replicated) double DownStartedAt=-1;
    UPROPERTY(Replicated) double RecoveryStartedAt=-1;
    UPROPERTY(Replicated) FGuid DownEpisodeId;
    UPROPERTY(Replicated) double HitReactionStartedAt=-1;
    FGuid FinishedStrikeId;
    bool bFinishedStrikeHit=false;
    TWeakObjectPtr<AActor> AttackTarget;
    TWeakObjectPtr<USkeletalMeshComponent> StrikeMesh;
    FDelegateHandle StrikePoseHandle;
    FVector PreviousHandPosition=FVector::ZeroVector;
    FVector PreviousOwnerPosition=FVector::ZeroVector;
    double PreviousHandTime=-1;
    uint8 PreviousMeshVisibility=0;
    bool bPreviousUpdateRateOptimization=false;
    bool bOverrideMeshTick=false;
    double RecoverAt=0;
    bool bAssistedRecovery=false;
    double UnassistedRecoverAt=0;
    TWeakObjectPtr<APawn> RecoveryHelper;
    FGuid CompletedRecoveryEpisode;
    TWeakObjectPtr<APawn> CompletedRecoveryHelper;
    double NextStrikeAt=0;
    bool bCurrentStrikeIsLight=false;
    FGuid FightActionId;
    TWeakObjectPtr<AActor> FightTarget;
    FVector FightOrigin=FVector::ZeroVector;
    float FightApproachOverride=0.f;
    double FightEndsAt=0;
    double NextFightAt=0;
    int32 FightPunches=0;
    int32 FightHits=0;
    bool bModelFight=false;
    int32 FightBurstPunches=0;
    int32 ReceivedLightHits=0;
    double LastLightHitAt=-100;
    TArray<FGuid> RecentLightHits;
};
