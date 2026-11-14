#pragma once

#include "CoreMinimal.h"

class ADeliveryCharacter;
class APlayerController;
class APlayerState;
class APawn;
class UWorld;

enum class EDeliverySpeechPlayerContextState : uint8 { Valid, Pending, Invalid };

// 连麦跟随同一玩家和原人物，上下车只改变受控 Pawn，不重新创建语音会话
// 只在游戏线程查询，复制暂未到齐时由持有者有界等待，不能据此提交服务器请求
struct DELIVERY_API FDeliverySpeechPlayerContext
{
    bool Capture(APlayerController* Player);
    EDeliverySpeechPlayerContextState Check(const APlayerController* Player) const;
    void Reset();
    bool IsCaptured() const;
    ADeliveryCharacter* GetCharacter() const;

    static constexpr double TransitionGraceSeconds = 2.0;

private:
    TWeakObjectPtr<APlayerController> Owner;
    TWeakObjectPtr<APlayerState> PlayerState;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<APawn> OriginalPawn;
};
