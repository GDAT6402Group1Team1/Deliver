#pragma once
#include "CoreMinimal.h"

// 只保存本次装载的目标和本组件创建的电机，不持有车辆或其他 World 的对象
struct FDeliveryTrunkBodyControl
{
    FName Bone;
    FName Control;
    FTransform Start;
    FTransform Folded;
    FTransform Unfolded;
    FTransform Target;
};
