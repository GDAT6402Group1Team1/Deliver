#pragma once
#include "CoreMinimal.h"

// 编辑器读取的一次姿态快照，不保存对象引用，也不修改玩法状态
struct FDeliveryPoseDebug
{
    FVector Hips = FVector::ZeroVector;
    FVector HipTarget = FVector::ZeroVector;
    FVector Right = FVector::RightVector;
    FVector Feet[2] = {};
    FVector FootTargets[2] = {};
    FVector Shoulders[2] = {};
    FVector Elbows[2] = {};
    FVector Hands[2] = {};
    float FootErrors[2] = {};
    float ArmErrors[2] = {};
    bool FootDriven[2] = {};
    bool ArmValid[2] = {};
    bool bFootLate = false;
    bool bCrossed = false;
    bool bGrounded = false;
    bool bLimp = false;
    bool bRising = false;
    bool bFootRecovery = false;
    bool bArmRecovery = false;
    bool bPunching = false;
    bool bReleased = false;
    bool bReturning = false;
    float Separation = 0.f;
    float Upright = 0.f;
    float MinimumUpright = 0.f;
};
