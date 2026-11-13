#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryNpcCombatComponent.generated.h"

// 在服务器处理 NPC 挥拳、受击和起身，同一拳只判定一次命中
// 倒地和起身状态同步给客户端，由表现组件播放相应动画
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcCombatComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryNpcCombatComponent();
    // 检查距离、遮挡和身体状态，攻击冷却在 StartStrike 中检查
    bool CanStrike(AActor* Target) const;
    // 近身可直接挥拳，较远的目标必须有完整导航路径才提供给模型
    bool CanApproachStrike(AActor* Target) const;
    // 接近途中检查目标和攻击冷却，实际路径由正在运行的导航任务检查
    bool CanContinueStrikeApproach(AActor* Target) const;
    // 返回 true 表示开始挥拳，是否命中要等到命中时间再判断
    bool StartStrike(AActor* Target,FGuid ActionId);
    // 模型选择持续交战后，由行为树调用；整轮固定目标、时间和最多挥拳次数
    bool CanBeginFight(AActor* Target) const;
    bool BeginFight(AActor* Target,FGuid ActionId,float Duration);
    bool StartFightStrike(AActor* Target,FGuid ActionId,FGuid PunchId);
    // 返回停止原因，空值表示还能继续；收拳与冷却由行为树等待
    FName GetFightStopReason(FGuid ActionId) const;
    bool IsReadyToStrike() const;
    void EndFight(FGuid ActionId);
    FString DescribeFight(FGuid ActionId,FName Reason) const;
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
    // 在服务器处理击退和倒地，不等待模型回复
    void ReceiveImpact(const FVector& Impulse,AActor* Source);
    bool SetDeliberateProne(bool bProne);
    UFUNCTION(BlueprintPure) bool IsDown() const { return bDown; }
    // 主动躺下保留说话能力，被击倒时仍立即停止对白
    bool IsDeliberatelyProne() const { return bDown && bDeliberate; }
    UFUNCTION(BlueprintPure) bool IsRecovering() const { return bRecovering; }
    UFUNCTION(BlueprintPure) bool IsStriking() const { return AttackId.IsValid(); }
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="50",ClampMax="250",Units="cm")) float StrikeRange=160;
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="160",ClampMax="1200",Units="cm")) float StrikeApproachRange=600;
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="1",ClampMax="10",Units="s")) float RecoverySeconds=4;
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="1",ClampMax="5")) int32 FightPunchLimit=3;
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="160",ClampMax="1200",Units="cm")) float FightTravelRange=600;
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="1",ClampMax="15",Units="s")) float FightCooldown=4;
    // 只累计持续交战中的轻拳，普通重击仍走原来的倒地流程
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="2",ClampMax="8")) int32 LightHitsToKnockDown=4;
    UPROPERTY(EditAnywhere,Category="Delivery|交战",meta=(ClampMin="2",ClampMax="20",Units="s")) float LightHitResetSeconds=10;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    bool CanStrikeTarget(AActor* Target,float MaxDistance) const;
    bool StartStrikeInternal(AActor* Target,FGuid PunchId,FGuid ActionId,bool bLightHit);
    void ReceiveFightHit(AActor* Source,FGuid PunchId);
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
    UPROPERTY(Replicated) double HitReactionStartedAt=-1;
    FGuid FinishedStrikeId;
    bool bFinishedStrikeHit=false;
    TWeakObjectPtr<AActor> AttackTarget;
    double HitAt=0;
    double RecoverAt=0;
    double NextStrikeAt=0;
    bool bCurrentStrikeIsLight=false;
    FGuid FightActionId;
    TWeakObjectPtr<AActor> FightTarget;
    FVector FightOrigin=FVector::ZeroVector;
    double FightEndsAt=0;
    double NextFightAt=0;
    int32 FightPunches=0;
    int32 FightHits=0;
    int32 ReceivedLightHits=0;
    double LastLightHitAt=-100;
    TArray<FGuid> RecentLightHits;
};
