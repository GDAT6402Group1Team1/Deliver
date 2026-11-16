#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

/// 检查玩家和 NPC 之间有没有东西挡着说话，交谈组件和 NPC 都用它
namespace DeliveryNpcConversationTrace
{
/// 现场交谈的最远距离
inline constexpr double LocalConversationRangeCm=600.;

/// @brief 从听的人到说的人之间有没有东西挡着
///
/// 玩家和 NPC 聊天时，常有路人从两人中间走过；如果把路人也算成遮挡，NPC 会突然听不到玩家说话
/// 所以射线碰到人物时把他加进忽略名单再射一次，最多 16 次；碰到墙、门、物件才算挡住
/// 出拳和交互不用这个函数，它们对遮挡的要求更严
/// @param Offset 两端同时抬高的量，用来从嘴的高度查
/// @return 中间没有墙和物件挡着时返回 true
inline bool HasClearPath(const AActor* Listener,const AActor* Speaker,const FVector& Offset=FVector::ZeroVector)
{
    if (!IsValid(Listener) || !IsValid(Speaker) || !Listener->GetWorld() ||
        Listener->GetWorld()!=Speaker->GetWorld()) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(NpcConversationPath),false,Listener);
    const FVector Start=Listener->GetActorLocation()+Offset;
    const FVector End=Speaker->GetActorLocation()+Offset;

    // 每次只排除真正挡在线上的人物，避免遍历关卡中全部行人
    for (int32 Index=0;Index<16;++Index)
    {
        FHitResult Hit;
        if (!Listener->GetWorld()->LineTraceSingleByChannel(Hit,Start,End,ECC_Visibility,Params) ||
            Hit.GetActor()==Speaker) return true;
        const AActor* Obstacle=Hit.GetActor();
        if (!IsValid(Obstacle) || !Obstacle->IsA<APawn>()) return false;
        Params.AddIgnoredActor(Obstacle);
    }
    return false;
}

/// @brief 能不能听见：在嘴的高度和身体中心各查一次，矮栏杆只挡住一条时仍算能听见
inline bool HasClearSpeechPath(const AActor* Listener,const AActor* Speaker)
{
    return HasClearPath(Listener,Speaker,FVector(0,0,40)) || HasClearPath(Listener,Speaker);
}
}
