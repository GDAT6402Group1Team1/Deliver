#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "DeliveryNpcBodyAnimInstance.generated.h"

class UAnimSequence;
class UCharacterMovementComponent;

/**
 * @brief NPC 主动画蓝图 ABP_NpcBody 的父类：每帧决定播哪个姿势、播到哪
 *
 * 选姿势：NativeUpdateAnimation 按起身 → 倒地 → 挨打 → 出拳 → 拽车 → 跳跃 → 走路 → 说话的顺序挑 PoseIndex
 * 对时间：出拳、倒地、起身、挨打按服务器开始时间算播放进度，迟到的客户端也能接上
 * 姿势之间的过渡在蓝图里配；转头和倾听由 ABP_NpcConversation 叠在上面
 */
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryNpcBodyAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
public:
    /// 所有姿势，顺序要和蓝图 Blend Poses 的输入一致
    enum class EPose : uint8 { Idle, Talk, Walk, Pace, Hurry, Hold, Strike, Down, GetUp, Hit, JumpStart, JumpLoop, JumpLand };

    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    virtual void NativeUninitializeAnimation() override;

    // ── 选姿势 ──
    // 蓝图读下面这些值：PoseIndex 选片段，各个 Time 定播到哪

    /// 当前姿势，索引顺序和居民、二狗主动画蓝图的 Blend Poses 输入一致
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") int32 PoseIndex=0;

    /// 走路动画的播放快慢，按实际速度算
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float MovePlayRate=1;

    /// 出拳播到第几秒
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float StrikeTime=0;

    /// 倒地播到第几秒
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float DownTime=0;

    /// 起身播到第几秒
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float GetUpTime=0;

    /// 挨打后仰播到第几秒
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float HitTime=0;

    /// 起跳播到第几秒
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float JumpStartTime=0;

    /// 空中循环播到第几秒
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float JumpLoopTime=0;

    /// 落地播到第几秒
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float JumpLandTime=0;

    /// @brief 是否在跳；跳跃时盖过家居动作的全身或上身叠加
    bool IsPlayingJumpPose() const { return PoseIndex>=static_cast<int32>(EPose::JumpStart); }

    // ── 配置 ──

    /// 超过这个速度才开始走；停下的门槛更低，低速时不会在站和走之间来回切
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|步态",meta=(ClampMin="1",Units="cm/s")) float MoveStartSpeed=24;

    /// 低于这个速度才算停下
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|步态",meta=(ClampMin="0",Units="cm/s")) float MoveStopSpeed=12;

    /// 站着；正式角色从表现组件取片段，这里的默认值给编辑器预览用
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> IdleAnimation;

    /// 说话
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> TalkAnimation;

    /// 走路
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> WalkAnimation;

    /// 踱步
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> PaceAnimation;

    /// 快步走
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> HurryAnimation;

    /// 拽车
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> HoldAnimation;

    /// 出拳
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> AttackAnimation;

    /// 倒地
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> DownAnimation;

    /// 起身
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> GetUpAnimation;

    /// 挨打后仰
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> HitAnimation;

    /// 起跳
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> JumpStartAnimation;

    /// 空中
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> JumpLoopAnimation;

    /// 落地
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> JumpLandAnimation;
private:
    // ── 选姿势 ──

    /// 按离地、上升、下落推进起跳 → 空中 → 落地
    EPose UpdateJumpPose(const UCharacterMovementComponent* Movement,float DeltaSeconds);

    /// 回到不跳的状态
    void ResetJumpPose();

    /// 上一帧在不在走
    bool bMoving=false;

    /// 跳跃进行到哪一段
    EPose JumpPose=EPose::Idle;

    /// 这一段已经播了多久
    float JumpPhaseSeconds=0;
};
