#pragma once

#include "CoreMinimal.h"

class ADeliveryCharacter;
class APlayerController;
class APlayerState;
class APawn;
class UWorld;

enum class EDeliverySpeechPlayerContextState : uint8 { Valid, Pending, Invalid };

/**
 * @brief 开麦时记下的玩家、PlayerState、世界和人物，之后每帧检查它们是否还对得上
 *
 * 连麦跟着同一个玩家和他原来的人物；上下车只换控制的 Pawn（人物或摩托车），语音不用重开
 * 只在游戏线程使用；上下车的同步还没到齐时 Check 返回 Pending，调用方最多等 TransitionGraceSeconds，这期间不能提交服务器请求
 */
struct DELIVERY_API FDeliverySpeechPlayerContext
{
    /// @brief 记下 Player 现在的 PlayerState、世界和人物（骑车时记车上的驾驶员）；记下后立刻 Check 一次，不是 Valid 就清掉并返回 false
    bool Capture(APlayerController* Player);

    /// @brief 对得上返回 Valid；上下车的控制关系还没同步完返回 Pending；换了玩家、人物被别人控制、世界没了返回 Invalid
    EDeliverySpeechPlayerContextState Check(const APlayerController* Player) const;

    /// @brief 清空记录
    void Reset();

    /// @brief 已经记下而且都还有效
    bool IsCaptured() const;

    /// @brief 记下的人物
    ADeliveryCharacter* GetCharacter() const;

    /// Check 连续返回 Pending 最多等多少秒，超过就当作 Invalid
    static constexpr double TransitionGraceSeconds = 2.0;

private:
    /// 开麦的玩家控制器
    TWeakObjectPtr<APlayerController> Owner;

    /// 开麦时的 PlayerState
    TWeakObjectPtr<APlayerState> PlayerState;

    /// 开麦时的世界
    TWeakObjectPtr<UWorld> World;

    /// 开麦时的人物（骑车时是车上的驾驶员）
    TWeakObjectPtr<APawn> OriginalPawn;
};
