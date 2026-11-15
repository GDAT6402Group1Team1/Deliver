#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "DeliveryNpcBodyAnimInstance.generated.h"

class UAnimSequence;
class UCharacterMovementComponent;

// 将 NPC 的动作、速度和服务器时间交给主动画蓝图，片段之间的过渡在蓝图中配置
// 注视与倾听仍由 ABP_NpcConversation 在完整身体姿势上叠加
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryNpcBodyAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
public:
    enum class EPose : uint8 { Idle, Talk, Walk, Pace, Hurry, Hold, Strike, Down, GetUp, Hit, JumpStart, JumpLoop, JumpLand };

    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    virtual void NativeUninitializeAnimation() override;

    // 索引顺序与居民和二狗主动画蓝图的 Blend Poses 输入一致
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") int32 PoseIndex=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float MovePlayRate=1;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float StrikeTime=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float DownTime=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float GetUpTime=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float HitTime=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float JumpStartTime=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float JumpLoopTime=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="NPC|动作") float JumpLandTime=0;

    // 跳跃主体姿势可覆盖家居全身或上身叠加
    bool IsPlayingJumpPose() const { return PoseIndex>=static_cast<int32>(EPose::JumpStart); }

    // 起步和停步使用不同门槛，低速时不会反复切换站立与行走
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|步态",meta=(ClampMin="1",Units="cm/s")) float MoveStartSpeed=24;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|步态",meta=(ClampMin="0",Units="cm/s")) float MoveStopSpeed=12;

    // 正式角色从表现组件取得片段，蓝图默认值同时供编辑器预览使用
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> IdleAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> TalkAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> WalkAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> PaceAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> HurryAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> HoldAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> AttackAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> DownAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> GetUpAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> HitAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> JumpStartAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> JumpLoopAnimation;
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="NPC|动画") TObjectPtr<UAnimSequence> JumpLandAnimation;
private:
    EPose UpdateJumpPose(const UCharacterMovementComponent* Movement,float DeltaSeconds);
    void ResetJumpPose();

    bool bMoving=false;
    EPose JumpPose=EPose::Idle;
    float JumpPhaseSeconds=0;
};
