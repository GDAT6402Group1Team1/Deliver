// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryMapSettings.h"

UDeliveryMapSettings::UDeliveryMapSettings()
{
	// 归到 项目设置 → 游戏 下。不设的话 DeveloperSettings 默认落在"插件"分类里，
	// 而这是游戏自己的设置、不是插件，找起来很费劲
	CategoryName = TEXT("Game");
}

float UDeliveryMapSettings::GetMapNorthWorldYaw()
{
	const UDeliveryMapSettings* Settings = GetDefault<UDeliveryMapSettings>();

	// 归一化在这里做一次，调用方就不用各自记得归一化；
	// 填 270 和填 −90 必须等价，否则"看着填对了却差一圈"
	return Settings ? FRotator::NormalizeAxis(Settings->MapNorthWorldYaw) : 0.f;
}
