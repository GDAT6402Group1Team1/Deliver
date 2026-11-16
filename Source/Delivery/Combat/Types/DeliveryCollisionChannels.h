// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

/**
 * @brief 本项目自定义的碰撞通道，和 Config/DefaultEngine.ini 里 DefaultChannelResponses 的顺序一一对应
 *
 * 改 ini 时这里要一起改；两边顺序对不上时，查询会扫到完全无关的东西，而两个文件单看都像是对的
 *
 * ECC_GameTraceChannel1 = Traffic_Road（道路与路口段的 Box，蓝图在用）
 * ECC_GameTraceChannel2 = Traffic_Intersection
 */
namespace DeliveryCollision
{
/**
 * 路人和居民 NPC 的受击查询体用的通道
 *
 * 不用玩家布娃娃的 PhysicsBody 通道：抓取候选搜索、拖人、摩托车贴地探测都在查 PhysicsBody，
 * 路人放在那个通道上会被它们一起扫到，出现能抓起路人、摩托车贴地贴到人头上这类问题，每一处都得单独排除
 * 单独一个通道后，要打路人的查询（出拳、撞车）显式查它
 */
static constexpr ECollisionChannel Pedestrian = ECC_GameTraceChannel3;
}  // namespace DeliveryCollision
