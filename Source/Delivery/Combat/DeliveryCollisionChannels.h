// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

/**
 * 本项目自定义的碰撞通道，名字和 Config/DefaultEngine.ini 的
 * `DefaultChannelResponses` 一一对应。**改 ini 就要改这里**，
 * 顺序错位的表现是"查询扫到了完全无关的东西"，而两边单看都对。
 *
 * ECC_GameTraceChannel1 = Traffic_Road（道路与路口段的 Box，蓝图在用）
 * ECC_GameTraceChannel2 = Traffic_Intersection
 */
namespace DeliveryCollision
{
/**
 * 路人专用通道
 *
 * **刻意不复用 PhysicsBody**（玩家布娃娃用的那个）：那个通道被一大堆现有查询
 * 盯着——抓取候选搜索、拖人、摩托车贴地探测都在查 PhysicsBody。
 * 路人挂上去的话会被它们全部捎上，表现是"能抓起路边的行人"、
 * "摩托车贴地贴到人头上"这类根本没人想要的联动，而且每一处都要单独去排除。
 * 自己一个通道，谁想打它就显式查它。
 */
static constexpr ECollisionChannel Pedestrian = ECC_GameTraceChannel3;
}  // namespace DeliveryCollision
