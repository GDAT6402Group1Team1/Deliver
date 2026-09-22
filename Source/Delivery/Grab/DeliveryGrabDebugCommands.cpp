#include "Delivery.h"

#if !UE_BUILD_SHIPPING

#include "DeliveryCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "Grab/DeliveryGrabComponent.h"
#include "HAL/IConsoleManager.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "TimerManager.h"

namespace DeliveryGrabDebug
{
	FTimerHandle PullTimer;
	FTimerHandle StopPullTimer;

	void Print(const FString& Message)
	{
		UE_LOG(LogDelivery, Display, TEXT("[GrabTest] %s"), *Message);
		if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Cyan, Message);
	}

	ADeliveryCharacter* LocalGrabber(UWorld* World)
	{
		if (!World || !World->IsGameWorld() || World->GetNetMode() == NM_Client) return nullptr;
		for (TActorIterator<ADeliveryCharacter> It(World); It; ++It)
		{
			if (It->HasAuthority() && It->IsLocallyControlled() && It->GetController()) return *It;
		}
		return nullptr;
	}

	ADeliveryCharacter* OtherPlayer(UWorld* World, const ADeliveryCharacter* Grabber)
	{
		for (TActorIterator<ADeliveryCharacter> It(World); It; ++It)
		{
			if (*It != Grabber && It->GetController()) return *It;
		}
		return nullptr;
	}

	void Setup(const TArray<FString>&, UWorld* World)
	{
		ADeliveryCharacter* Grabber = LocalGrabber(World);
		ADeliveryCharacter* Target = Grabber ? OtherPlayer(World, Grabber) : nullptr;
		if (!Grabber || !Target)
		{
			Print(TEXT("需要双人 listen-server PIE，并在服务器玩家窗口执行命令。"));
			return;
		}
		if (UDeliveryGrabComponent* Grab = Grabber->GetGrabComponent()) Grab->ForceRelease();
		Target->GetActiveRagdoll()->SetLimp(true);
		USkeletalMeshComponent* TargetMesh = Target->GetMesh();
		FBodyInstance* Root = TargetMesh->GetBodyInstance();
		if (!Root)
		{
			Print(TEXT("目标没有物理根刚体。"));
			return;
		}
		const float Yaw = Grabber->GetController()->GetControlRotation().Yaw;
		const FVector Forward = FRotator(0.0f, Yaw, 0.0f).Vector();
		const FVector GrabberHips = Grabber->GetMesh()->GetBoneLocation(TEXT("Hips"));
		const FVector TargetHips = TargetMesh->GetBoneLocation(TEXT("Hips"));
		TargetMesh->SetAllPhysicsPosition(Root->GetUnrealWorldTransform().GetLocation()
			+ GrabberHips + Forward * 105.0f - TargetHips);
		Target->ForceNetUpdate();
		Grabber->GetController()->SetControlRotation(FRotator(-18.0f, Yaw, 0.0f));
		Print(FString::Printf(TEXT("已将 %s 放在 %s 前方并设为 Limp。"),
			*Target->GetName(), *Grabber->GetName()));
	}

	void Status(const TArray<FString>&, UWorld* World)
	{
		ADeliveryCharacter* Grabber = LocalGrabber(World);
		if (!Grabber) { Print(TEXT("未找到服务器本地玩家。")); return; }
		const UDeliveryGrabComponent* Grab = Grabber->GetGrabComponent();
		ADeliveryCharacter* Target = Grab ? Cast<ADeliveryCharacter>(Grab->GrabTarget.Get()) : nullptr;
		const FVector Hand = Grabber->GetMesh()->GetBoneLocation(
			Grab && (Grab->AttachedHands & 2) ? TEXT("RightHand") : TEXT("LeftHand"));
		Print(FString::Printf(TEXT("target=%s hands=%u gapL=%.1f gapR=%.1f hand=%s targetHips=%s"),
			*GetNameSafe(Target), Grab ? Grab->AttachedHands : 0,
			Grab ? Grab->LeftHandGap : 0.0f, Grab ? Grab->RightHandGap : 0.0f,
			*Hand.ToCompactString(),
			Target ? *Target->GetMesh()->GetBoneLocation(TEXT("Hips")).ToCompactString() : TEXT("none")));
	}

	void Grab(const TArray<FString>&, UWorld* World)
	{
		ADeliveryCharacter* Grabber = LocalGrabber(World);
		if (!Grabber) { Print(TEXT("未找到服务器本地玩家。")); return; }
		UDeliveryGrabComponent* Component = Grabber->GetGrabComponent();
		AActor* Candidate = nullptr;
		FVector Point;
		const bool bFound = Component->DebugFindCandidate(Candidate, Point);
		Print(FString::Printf(TEXT("候选=%s point=%s"),
			bFound ? *GetNameSafe(Candidate) : TEXT("none"), *Point.ToCompactString()));
		Component->RequestBegin();
		Status({}, World);
	}

	void Pull(const TArray<FString>&, UWorld* World)
	{
		ADeliveryCharacter* Grabber = LocalGrabber(World);
		if (!Grabber) { Print(TEXT("未找到服务器本地玩家。")); return; }
		World->GetTimerManager().ClearTimer(PullTimer);
		World->GetTimerManager().ClearTimer(StopPullTimer);
		TWeakObjectPtr<ADeliveryCharacter> WeakGrabber(Grabber);
		World->GetTimerManager().SetTimer(PullTimer,
			FTimerDelegate::CreateLambda([WeakGrabber]()
			{
				if (ADeliveryCharacter* Character = WeakGrabber.Get()) Character->DoMove(0.0f, -1.0f);
			}), 0.05f, true);
		World->GetTimerManager().SetTimer(StopPullTimer,
			FTimerDelegate::CreateLambda([World, WeakGrabber]()
			{
				World->GetTimerManager().ClearTimer(PullTimer);
				if (ADeliveryCharacter* Character = WeakGrabber.Get()) Character->DoMove(0.0f, 0.0f);
				Status({}, World);
			}), 2.0f, false);
		Print(TEXT("向后移动 2 秒，然后报告拖拽状态。"));
	}

	void Release(const TArray<FString>&, UWorld* World)
	{
		if (ADeliveryCharacter* Grabber = LocalGrabber(World))
		{
			Grabber->GetGrabComponent()->ForceRelease();
			Print(TEXT("已释放测试抓取。"));
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs GDeliveryGrabSetupCmd(TEXT("Delivery.Grab.TestSetup"),
	TEXT("双人 PIE：让另一位玩家倒在本机玩家前方。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryGrabDebug::Setup));
static FAutoConsoleCommandWithWorldAndArgs GDeliveryGrabTryCmd(TEXT("Delivery.Grab.TestGrab"),
	TEXT("使用真实的候选查询和服务端抓取流程测试拖人。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryGrabDebug::Grab));
static FAutoConsoleCommandWithWorldAndArgs GDeliveryGrabStatusCmd(TEXT("Delivery.Grab.TestStatus"),
	TEXT("报告抓取目标、手部连接和位置。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryGrabDebug::Status));
static FAutoConsoleCommandWithWorldAndArgs GDeliveryGrabPullCmd(TEXT("Delivery.Grab.TestPull"),
	TEXT("抓取后向后移动两秒并报告结果。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryGrabDebug::Pull));
static FAutoConsoleCommandWithWorldAndArgs GDeliveryGrabReleaseCmd(TEXT("Delivery.Grab.TestRelease"),
	TEXT("断开测试抓取。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryGrabDebug::Release));

#endif
