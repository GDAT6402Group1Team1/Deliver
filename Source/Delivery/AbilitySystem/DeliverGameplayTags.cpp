// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/DeliverGameplayTags.h"

// 左右拳技能的标签，输入组件按标签调用 TryActivateAbilitiesByTag 出拳
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Attack_Punch_Left, "Ability.Attack.Punch.Left");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Attack_Punch_Right, "Ability.Attack.Punch.Right");

// 伤害效果 GE_Damage 的 SetByCaller 标签，GA_DeliverPunch 用它传入扣多少血
UE_DEFINE_GAMEPLAY_TAG(TAG_Effect_Type_Damage, "Effect.Type.Damage");

// 晕倒状态，技能组件的 SetStunned 挂上；带着它不能出拳
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Stunned, "State.Stunned");

// 小绿相关的玩法事件
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Delivery_Caterpillar, "Event.Delivery.Caterpillar");
