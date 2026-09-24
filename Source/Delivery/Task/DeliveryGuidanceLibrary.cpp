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
