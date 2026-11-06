// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * 输入链路的现场快照：Delivery.Input.Dump
 *
 * 为什么要有这条命令：
 * "下车之后前后左右和鼠标全不管用、人还一直往前走" 这种症状，至少有四个互不相干的
 * 成因会长得一模一样——控制器根本没占有这具身体、身体没绑上 InputComponent、
 * 光标开着把鼠标吃掉了、或者按键其实进来了但布娃娃不处在能走的控制模式。
 * 在 PIE 里靠轮询属性猜要花很久，而这四件事各自都只是一个布尔量，
 * 一次全列出来就能当场分清。只读，不改任何状态。
 */

#if !UE_BUILD_SHIPPING

#include "Character/DeliveryCharacter.h"
#include "Components/InputComponent.h"
#include "Delivery.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Input/DeliveryInputComponent.h"
#include "Player/DeliveryPlayerController.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "UObject/UObjectIterator.h"
#include "Vehicle/DeliveryMotorbike.h"

namespace DeliveryInputDebug
{
void Print(const FString &Message)
{
	UE_LOG(LogDelivery, Display, TEXT("%s"), *Message);

	// 卡住的时候玩家多半是先看屏幕再看日志，两边都打一份
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.f, FColor::Yellow, Message);
	}
}

const TCHAR *ControlModeToString(EDeliveryRagdollControlMode Mode)
{
	switch (Mode)
	{
		case EDeliveryRagdollControlMode::Disabled:
			return TEXT("Disabled（没在模拟）");
		case EDeliveryRagdollControlMode::Active:
			return TEXT("Active（能走）");
		case EDeliveryRagdollControlMode::Limp:
			return TEXT("Limp（瘫软，走不了）");
		case EDeliveryRagdollControlMode::Recovering:
			return TEXT("Recovering（起身中，走不了）");
		default:
			return TEXT("未知");
	}
}

void DumpCharacter(ADeliveryCharacter *Character, const TCHAR *Role, APlayerController *PC)
{
	if (!Character)
	{
		Print(FString::Printf(TEXT("[Input] %s：没有。"), Role));
		return;
	}

	Print(FString::Printf(TEXT("[Input] %s = %s"), Role, *Character->GetName()));
	Print(FString::Printf(TEXT("  本机操控=%s  控制器=%s  这具身体的 InputComponent=%s"),
	                      Character->IsLocallyControlled() ? TEXT("是") : TEXT("否"),
	                      *GetNameSafe(Character->GetController()),
	                      Character->InputComponent ? *Character->InputComponent->GetName() : TEXT("空！")));

	// 绑没绑上按键，是"所有键都没反应"最直接的判据
	const UDeliveryInputComponent *Input = Character->GetPlayerInput();
	const UInputComponent *Bound = Input ? Input->GetBoundInputForDebug() : nullptr;
	Print(FString::Printf(TEXT("  按键已绑到：%s%s"), Bound ? *Bound->GetName() : TEXT("没绑！"),
	                      (Bound && Character->InputComponent && Bound != Character->InputComponent)
	                          ? TEXT("  ← 和身体当前的 InputComponent 不是同一个，绑的是已经作废的那个")
	                          : TEXT("")));

	if (const UDeliveryActiveRagdollComponent *Ragdoll = Character->GetActiveRagdoll())
	{
		const FVector2D Move = Ragdoll->GetMoveInputForDebug();
		Print(FString::Printf(TEXT("  布娃娃：模拟中=%s  控制模式=%s  移动意图=(%.2f, %.2f)%s"),
		                      Ragdoll->IsRagdollActive() ? TEXT("是") : TEXT("否"),
		                      ControlModeToString(Ragdoll->GetControlMode()), Move.X, Move.Y,
		                      Move.IsNearlyZero() ? TEXT("")
		                                          : TEXT("  ← 没按键却非零 = 有一次松开事件丢了")));
	}

	Print(FString::Printf(TEXT("  晕倒中=%s  当前载具=%s"),
	                      Character->IsIncapacitated() ? TEXT("是") : TEXT("否"),
	                      *GetNameSafe(Character->GetCurrentVehicle())));

	if (PC && PC->GetPawn() != Character)
	{
		Print(TEXT("  ！控制器现在占有的不是这具身体，按键不会送到这里来。"));
	}
}

void Dump(UWorld *World)
{
	if (!World)
	{
		Print(TEXT("[Input] 没有世界，先进 PIE。"));
		return;
	}

	APlayerController *PC = World->GetFirstPlayerController();
	if (!PC)
	{
		Print(TEXT("[Input] 没有玩家控制器。"));
		return;
	}

	APawn *Pawn = PC->GetPawn();
	Print(FString::Printf(TEXT("[Input] 控制器 %s 现在占有：%s（%s）"), *PC->GetName(),
	                      *GetNameSafe(Pawn), Pawn ? *Pawn->GetClass()->GetName() : TEXT("无")));

	// 光标开着时鼠标移动归光标、不归视角——"鼠标转不动视角"的头号原因
	Print(FString::Printf(TEXT("  光标=%s  移动输入被忽略=%s  视角输入被忽略=%s"),
	                      PC->bShowMouseCursor ? TEXT("显示中（鼠标会被光标吃掉）") : TEXT("隐藏"),
	                      PC->IsMoveInputIgnored() ? TEXT("是！") : TEXT("否"),
	                      PC->IsLookInputIgnored() ? TEXT("是！") : TEXT("否")));

	if (const ADeliveryPlayerController *Ours = Cast<ADeliveryPlayerController>(PC))
	{
		const TSet<FName> &Panels = Ours->GetOpenUIPanelsForDebug();
		FString Names;
		for (const FName &Panel : Panels)
		{
			Names += (Names.IsEmpty() ? TEXT("") : TEXT("、")) + Panel.ToString();
		}
		Print(FString::Printf(TEXT("  开着的面板：%s  手机面板标记=%s"),
		                      Panels.Num() > 0 ? *Names : TEXT("（无）"),
		                      Ours->IsPhoneUIOpen() ? TEXT("开") : TEXT("关")));
	}

	// 直接被占有的身体，和"坐在车上那具被隐藏的身体"要分开看
	ADeliveryCharacter *Direct = Cast<ADeliveryCharacter>(Pawn);
	ADeliveryMotorbike *Bike = Cast<ADeliveryMotorbike>(Pawn);
	DumpCharacter(Direct ? Direct : (Bike ? Bike->GetDriver() : nullptr),
	              Direct ? TEXT("被占有的角色") : TEXT("车上的驾驶员"), PC);

	// 车就算没人骑也要看：残余车速会把刚下车的人一路顶着走
	for (TObjectIterator<ADeliveryMotorbike> It; It; ++It)
	{
		ADeliveryMotorbike *Found = *It;
		if (!IsValid(Found) || Found->GetWorld() != World)
		{
			continue;
		}
		Print(FString::Printf(TEXT("[Input] 摩托车 %s：驾驶员=%s  车速=%.0f cm/s%s"), *Found->GetName(),
		                      *GetNameSafe(Found->GetDriver()), Found->GetCurrentSpeed(),
		                      (!Found->GetDriver() && FMath::Abs(Found->GetCurrentSpeed()) > 1.f)
		                          ? TEXT("  ← 没人骑却还在动，会顶到刚下车的人")
		                          : TEXT("")));
	}
}

FAutoConsoleCommandWithWorld DumpCmd(TEXT("Delivery.Input.Dump"),
                                     TEXT("列出当前输入链路：占有关系、按键绑定、光标、布娃娃控制模式、残余车速"),
                                     FConsoleCommandWithWorldDelegate::CreateStatic(&Dump));
} // namespace DeliveryInputDebug

#endif
