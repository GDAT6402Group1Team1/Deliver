// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * 路人路线的调试命令：列出线段配置与在场路人、手动补人、清场
 * 和取货点那几条命令同一个用途——"摆了点却一个人都不出现"是最难查的空配置，
 * 所以这里一次把所有可能的原因列全，而不是遇到第一个就停
 */

#if !UE_BUILD_SHIPPING

#include "Components/SplineComponent.h"
#include "Delivery.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "NPC/DeliveryPedestrian.h"
#include "NPC/DeliveryPedestrianRoute.h"

namespace DeliveryPedestrianDebug
{
void Print(const FString& Message)
{
	UE_LOG(LogDelivery, Display, TEXT("%s"), *Message);

	// PIE 里日志窗口经常被别的东西刷掉，屏幕上再打一份
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Cyan, Message);
	}
}

const TCHAR* PhaseToString(EDeliveryPedestrianPhase Phase)
{
	switch (Phase)
	{
		case EDeliveryPedestrianPhase::Appearing:
			return TEXT("出现中");
		case EDeliveryPedestrianPhase::Walking:
			return TEXT("行走");
		case EDeliveryPedestrianPhase::Disappearing:
			return TEXT("消失中");
		case EDeliveryPedestrianPhase::KnockedDown:
			return TEXT("被打飞");
		case EDeliveryPedestrianPhase::StandingUp:
			return TEXT("起身中");
		default:
			return TEXT("未知");
	}
}

UWorld* ResolveWorld(UWorld* ContextWorld)
{
	return ContextWorld;
}

void Dump(UWorld* World)
{
	if (!World)
	{
		Print(TEXT("[Pedestrian] 没有世界，先进 PIE。"));
		return;
	}

	int32 RouteCount = 0;
	for (TActorIterator<ADeliveryPedestrianRoute> It(World); It; ++It)
	{
		ADeliveryPedestrianRoute* Route = *It;
		++RouteCount;

		const float Length = Route->GetRouteLength();
		const int32 PointCount = Route->GetSpline() ? Route->GetSpline()->GetNumberOfSplinePoints() : 0;

		Print(FString::Printf(TEXT("[Pedestrian] 路线 %s：%d 个点、长 %.0f cm、上限 %d 人、在场 %d 人"),
		                      *Route->GetName(), PointCount, Length, Route->MaxAlive,
		                      Route->GetAlivePedestrians().Num()));

		// 一个人都不出现的三种原因，逐条点名而不是只报"没人"
		if (Length <= KINDA_SMALL_NUMBER)
		{
			Print(TEXT("  ！线段长度为 0：两个端点重合了，拖开才会有人。"));
		}

		// 接驳情况。"路人走到路口就消失"最常见的原因就是端点差了一点没判成连通，
		// 而那个和"本来就该在这儿退场"长得一模一样
		FVector StartPoint = FVector::ZeroVector;
		FVector FinishPoint = FVector::ZeroVector;
		Route->GetEndpoints(StartPoint, FinishPoint);
		Print(FString::Printf(TEXT("  端点 起(%.0f, %.0f) 终(%.0f, %.0f)；接驳%s，阈值 %.0f cm，上限 %d 次"),
		                      StartPoint.X, StartPoint.Y, FinishPoint.X, FinishPoint.Y,
		                      Route->bAllowHandoff ? TEXT("开") : TEXT("关"), Route->ConnectDistance,
		                      Route->MaxHandoffs));
		Print(FString::Printf(TEXT("  起点端下家：%s；终点端下家：%s"),
		                      Route->HasContinuation(false) ? TEXT("有") : TEXT("没有（走到这头就退场）"),
		                      Route->HasContinuation(true) ? TEXT("有") : TEXT("没有（走到这头就退场）")));

		int32 UsableKinds = 0;
		for (int32 Index = 0; Index < Route->Kinds.Num(); ++Index)
		{
			const FDeliveryPedestrianKind& Kind = Route->Kinds[Index];
			const FString Path = Kind.Mesh.IsNull() ? TEXT("（空）") : Kind.Mesh.ToString();
			const bool bUsable = !Kind.Mesh.IsNull() && Kind.Weight > 0.f;
			if (bUsable)
			{
				++UsableKinds;
			}
			Print(FString::Printf(TEXT("  外观 %d：权重 %.2f %s %s"), Index, Kind.Weight, *Path,
			                      bUsable ? TEXT("") : TEXT("← 不会出现")));
		}

		if (UsableKinds == 0)
		{
			Print(TEXT("  ！没有可用外观：Kinds 为空或权重全为零，路线会一直是空的。"));
		}

		if (Route->WalkAnim.IsNull())
		{
			Print(TEXT("  走路动画留空：人会走但没有腿部动作（滑步），不是错误。"));
		}
		else
		{
			Print(FString::Printf(TEXT("  走路动画：%s（基准速度 %.0f cm/s）"),
			                      *Route->WalkAnim.ToString(), Route->WalkAnimReferenceSpeed));
		}

		for (const TWeakObjectPtr<ADeliveryPedestrian>& Entry : Route->GetAlivePedestrians())
		{
			const ADeliveryPedestrian* Pedestrian = Entry.Get();
			if (!Pedestrian)
			{
				continue;
			}

			Print(FString::Printf(
			    TEXT("    %s：%.0f/%.0f cm、%s、%.0f cm/s、缩放 %.2f、%s%s"), *Pedestrian->GetName(),
			    Pedestrian->GetDistanceAlongRoute(), Length,
			    Pedestrian->IsWalkingForward() ? TEXT("正向") : TEXT("反向"), Pedestrian->GetWalkSpeed(),
			    Pedestrian->GetAppearAlpha(), PhaseToString(Pedestrian->GetPhase()),
			    Pedestrian->IsYielding()
			        ? *FString::Printf(TEXT("（正在让路，前方%s）"),
			                           Pedestrian->IsBlockedAhead() ? TEXT("有人") : TEXT("已空"))
			        : Pedestrian->IsStandingUp()
			        ? *FString::Printf(TEXT("（起身 %.0f%%）"), Pedestrian->GetGetUpAlpha() * 100.f)
			        : Pedestrian->IsKnockedDown()
			        ? *FString::Printf(TEXT("（还剩 %.1f 秒起身）"), Pedestrian->GetKnockedRemaining())
			        : (Pedestrian->IsPaused()
			               ? *FString::Printf(TEXT("（停着，还剩 %.1f 秒）换线 %d 次"),
			                                  Pedestrian->GetPauseRemaining(),
			                                  Pedestrian->GetHandoffCount())
			               : *FString::Printf(TEXT("，换线 %d 次"), Pedestrian->GetHandoffCount()))));
		}
	}

	if (RouteCount == 0)
	{
		Print(TEXT("[Pedestrian] 这张图里没有 ADeliveryPedestrianRoute，先跑 place_pedestrian_route.py 摆一条。"));
	}
}

void SpawnOne(UWorld* World)
{
	if (!World)
	{
		return;
	}

	int32 Spawned = 0;
	for (TActorIterator<ADeliveryPedestrianRoute> It(World); It; ++It)
	{
		// 跳生成间隔和人数上限，但仍走真实的生成路径，
		// 这样"手动能出、自动不出"才能区分出是节奏问题还是配置问题
		if (It->SpawnOnePedestrian(true))
		{
			++Spawned;
		}
	}

	Print(FString::Printf(TEXT("[Pedestrian] 手动补了 %d 个路人。"), Spawned));
}

void Clear(UWorld* World)
{
	if (!World)
	{
		return;
	}

	int32 Removed = 0;
	for (TActorIterator<ADeliveryPedestrian> It(World); It; ++It)
	{
		It->Destroy();
		++Removed;
	}

	Print(FString::Printf(TEXT("[Pedestrian] 清掉 %d 个路人。"), Removed));
}

void KnockNearest(UWorld* World)
{
	if (!World)
	{
		return;
	}

	// 从本机玩家位置找最近的那个，模拟一次被打飞——
	// 不用真去揍：手动验收要同时满足"站得到跟前""时机对上命中窗口"，
	// 而这条命令只验"被打飞之后那一串表现对不对"
	const APlayerController* PC = World->GetFirstPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	const FVector From = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;

	ADeliveryPedestrian* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();
	for (TActorIterator<ADeliveryPedestrian> It(World); It; ++It)
	{
		if (It->IsKnockedDown())
		{
			continue;
		}
		const float DistSq = FVector::DistSquared(It->GetActorLocation(), From);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = *It;
		}
	}

	if (!Best)
	{
		Print(TEXT("[Pedestrian] 附近没有还站着的路人。"));
		return;
	}

	if (!Best->HasAuthority())
	{
		// 受击只在服务器结算，客户端上调用会静默什么都不做
		Print(TEXT("[Pedestrian] 只有服务器能结算受击，在监听服务器那一端执行。"));
		return;
	}

	FVector Direction = Pawn ? (Best->GetActorLocation() - From).GetSafeNormal2D() : FVector::ForwardVector;
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::ForwardVector;
	}
	const FVector Impulse = (Direction + FVector::UpVector * 0.3f).GetSafeNormal() * 40000.f;
	const FVector Point = Best->GetActorLocation() + FVector(0.f, 0.f, 110.f);
	Best->ReceiveImpact(Impulse, Point);

	Print(FString::Printf(TEXT("[Pedestrian] 打飞 %s（距 %.0f cm）。"), *Best->GetName(),
	                      FMath::Sqrt(BestDistSq)));
}

FAutoConsoleCommandWithWorld DumpCmd(
    TEXT("Delivery.Pedestrian.Dump"),
    TEXT("列出所有人行路线的配置、在场路人以及不出人的原因"),
    FConsoleCommandWithWorldDelegate::CreateStatic(&Dump));

FAutoConsoleCommandWithWorld SpawnCmd(
    TEXT("Delivery.Pedestrian.Spawn"),
    TEXT("在每条人行路线上立刻补一个路人，跳过间隔和人数上限"),
    FConsoleCommandWithWorldDelegate::CreateStatic(&SpawnOne));

FAutoConsoleCommandWithWorld ClearCmd(
    TEXT("Delivery.Pedestrian.Clear"),
    TEXT("清掉当前所有路人"),
    FConsoleCommandWithWorldDelegate::CreateStatic(&Clear));

FAutoConsoleCommandWithWorld KnockCmd(
    TEXT("Delivery.Pedestrian.Knock"),
    TEXT("把离玩家最近的路人打飞，用来验收受击表现（服务器执行）"),
    FConsoleCommandWithWorldDelegate::CreateStatic(&KnockNearest));
}  // namespace DeliveryPedestrianDebug

#endif  // !UE_BUILD_SHIPPING
