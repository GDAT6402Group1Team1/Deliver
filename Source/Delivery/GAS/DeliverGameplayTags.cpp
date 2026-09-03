// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliverGameplayTags.h"

// 左右拳 Ability 激活用 Tag，输入层 TryActivateAbilitiesByTag 会用到。
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Attack_Punch_Left, "Ability.Attack.Punch.Left");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Attack_Punch_Right, "Ability.Attack.Punch.Right");

// GE_Damage 的 SetByCaller 幅度 Tag，GA 扣血时传入具体数值。
UE_DEFINE_GAMEPLAY_TAG(TAG_Effect_Type_Damage, "Effect.Type.Damage");

// 晕倒状态；拥有此 Tag 时不能挥拳（后续 GE_Stunned 挂上）。
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Stunned, "State.Stunned");
