#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

namespace DeliveryNpcConversationTrace
{
// 交谈时有人从中间走过，NPC 仍能听见和看向对方；墙壁、门和场景物件继续阻挡
// 出拳命中和玩家交互保留各自的严格检查，不使用这项交谈查询
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
}
