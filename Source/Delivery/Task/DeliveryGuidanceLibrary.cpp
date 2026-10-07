// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryGuidanceLibrary.h"

#include "DeliveryTaskTrackerComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

#define LOCTEXT_NAMESPACE "DeliveryGuidance"

namespace
{
	/** 本地玩家的镜头位置和朝向。拿不到返回 false。 */
	bool GetViewPoint(const UObject* WorldContextObject, FVector& OutLocation, FRotator& OutRotation)
	{
		if (!WorldContextObject || !GEngine)
		{
			return false;
		}

		const UWorld* World = GEngine->GetWorldFromContextObject(
			WorldContextObject, EGetWorldErrorMode::ReturnNull);
		APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		if (!Controller)
		{
			return false;
		}

		// 用镜头而不是 Pawn：玩家看的是镜头，而骑车时镜头和车头还可能不一致
		// （摩托车有自由视角/固定视角两套）。用 Pawn 朝向算的话自由视角下箭头会指错
		Controller->GetPlayerViewPoint(OutLocation, OutRotation);

		return true;
	}

	const UDeliveryTaskTrackerComponent* FindLocalTracker(const UObject* WorldContextObject)
	{
		if (!WorldContextObject || !GEngine)
		{
			return nullptr;
		}

		const UWorld* World = GEngine->GetWorldFromContextObject(
			WorldContextObject, EGetWorldErrorMode::ReturnNull);
		const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		const APlayerState* State = Controller ? Controller->PlayerState : nullptr;

		return State ? State->FindComponentByClass<UDeliveryTaskTrackerComponent>() : nullptr;
	}

	void MakeCompassCardinal(float CardinalWorldYaw, float ViewYaw, float HalfVisibleAngle,
		float& OutOffset, bool& bOutVisible)
	{
		const float RelativeYaw = FRotator::NormalizeAxis(CardinalWorldYaw - ViewYaw);
		const float UnclampedOffset = RelativeYaw / HalfVisibleAngle;
		bOutVisible = FMath::Abs(UnclampedOffset) <= 1.f;
		// 和任务目标使用同一种坐标约定：超出罗盘显示角度后停在左右边缘，
		// 不能继续给 UMG 一个大于 1 的位置让图标直接跑出 Canvas。
		OutOffset = FMath::Clamp(UnclampedOffset, -1.f, 1.f);
	}
}

FDeliveryGuidance UDeliveryGuidanceLibrary::MakeGuidanceToLocation(const UObject* WorldContextObject,
	FVector WorldLocation)
{
	FDeliveryGuidance Result;

	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	if (!GetViewPoint(WorldContextObject, ViewLocation, ViewRotation))
	{
		return Result;
	}

	const FVector Delta = WorldLocation - ViewLocation;

	Result.bValid = true;
	Result.WorldLocation = WorldLocation;
	Result.Distance = Delta.Size();
	Result.HeightOffset = Delta.Z;

	// 水平面上的夹角。带上 Z 算的话，目标在正下方时角度会乱跳，
	// 而屏幕边缘箭头本来就只需要"往左还是往右"
	const FVector2D FlatDelta(Delta.X, Delta.Y);
	if (FlatDelta.IsNearlyZero())
	{
		// 正好站在目标点上：角度没有意义，给 0 并当成在前方
		Result.RelativeYaw = 0.f;
		Result.bInFront = true;

		return Result;
	}

	const float TargetYaw = FMath::RadiansToDegrees(FMath::Atan2(FlatDelta.Y, FlatDelta.X));
	Result.RelativeYaw = FRotator::NormalizeAxis(TargetYaw - ViewRotation.Yaw);
	Result.bInFront = FMath::Abs(Result.RelativeYaw) < 90.f;

	return Result;
}

FDeliveryGuidance UDeliveryGuidanceLibrary::GetTaskGuidance(const UObject* WorldContextObject,
	const UDeliveryTaskDefinition* Task)
{
	FDeliveryGuidance Result;

	const UDeliveryTaskTrackerComponent* Tracker = FindLocalTracker(WorldContextObject);
	if (!Tracker)
	{
		return Result;
	}

	FVector Destination = FVector::ZeroVector;
	FName LocationId = NAME_None;
	if (!Tracker->GetTaskDestination(Task, Destination, LocationId))
	{
		return Result;
	}

	Result = MakeGuidanceToLocation(WorldContextObject, Destination);
	Result.LocationId = LocationId;

	return Result;
}

FDeliveryGuidance UDeliveryGuidanceLibrary::GetTrackedTaskGuidance(const UObject* WorldContextObject)
{
	const UDeliveryTaskTrackerComponent* Tracker = FindLocalTracker(WorldContextObject);

	return Tracker ? GetTaskGuidance(WorldContextObject, Tracker->GetTrackedTask()) : FDeliveryGuidance();
}

FDeliveryCompassGuidance UDeliveryGuidanceLibrary::GetTrackedTaskCompass(
	const UObject* WorldContextObject, float VisibleAngle, float NorthWorldYaw)
{
	FDeliveryCompassGuidance Result;

	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	if (!GetViewPoint(WorldContextObject, ViewLocation, ViewRotation))
	{
		return Result;
	}

	const float HalfVisibleAngle = FMath::Max(FMath::Abs(VisibleAngle) * 0.5f, 1.f);
	const float ViewYaw = FRotator::NormalizeAxis(ViewRotation.Yaw);
	const float NorthYaw = FRotator::NormalizeAxis(NorthWorldYaw);

	Result.bValid = true;
	// 世界里的 yaw 是从 +X 顺时针朝 +Y 增长。减掉地图北方后，正好就是
	// 玩家熟悉的 N=0 / E=90 / S=180 / W=270。
	Result.HeadingDegrees = FMath::Fmod(ViewYaw - NorthYaw + 360.f, 360.f);

	MakeCompassCardinal(NorthYaw, ViewYaw, HalfVisibleAngle,
		Result.NorthOffset, Result.bNorthVisible);
	MakeCompassCardinal(NorthYaw + 90.f, ViewYaw, HalfVisibleAngle,
		Result.EastOffset, Result.bEastVisible);
	MakeCompassCardinal(NorthYaw + 180.f, ViewYaw, HalfVisibleAngle,
		Result.SouthOffset, Result.bSouthVisible);
	MakeCompassCardinal(NorthYaw - 90.f, ViewYaw, HalfVisibleAngle,
		Result.WestOffset, Result.bWestVisible);

	const FDeliveryGuidance Guidance = GetTrackedTaskGuidance(WorldContextObject);
	if (Guidance.bValid)
	{
		Result.bHasTarget = true;
		Result.TargetRelativeYaw = Guidance.RelativeYaw;
		const float UnclampedOffset = Guidance.RelativeYaw / HalfVisibleAngle;
		Result.bTargetInStrip = FMath::Abs(UnclampedOffset) <= 1.f;
		Result.TargetOffset = FMath::Clamp(UnclampedOffset, -1.f, 1.f);
	}

	return Result;
}

FText UDeliveryGuidanceLibrary::FormatDistance(float Centimeters)
{
	const float Meters = Centimeters / 100.f;

	if (Meters < 1000.f)
	{
		return FText::Format(LOCTEXT("DistanceMeters", "{0} 米"),
			FText::AsNumber(FMath::RoundToInt(Meters)));
	}

	// 一公里以上保留一位小数：整数显示会让"1 公里"覆盖 950~1050 米这么大一段，
	// 玩家看着距离半天不变
	FNumberFormattingOptions Options;
	Options.MinimumFractionalDigits = 1;
	Options.MaximumFractionalDigits = 1;

	return FText::Format(LOCTEXT("DistanceKilometers", "{0} 公里"),
		FText::AsNumber(Meters / 1000.f, &Options));
}

#undef LOCTEXT_NAMESPACE
