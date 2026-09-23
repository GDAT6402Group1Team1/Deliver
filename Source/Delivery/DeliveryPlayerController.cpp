// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryPlayerController.h"
#include "Delivery.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Task/DeliveryPhoneCallQueueComponent.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryPlayerController::ADeliveryPlayerController()
{
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultIMC(TEXT("/Game/Input/IMC_Default"));
	if (DefaultIMC.Succeeded())
	{
		DefaultMappingContexts.Add(DefaultIMC.Object);
	}

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookIMC(TEXT("/Game/Input/IMC_MouseLook"));
	if (MouseLookIMC.Succeeded())
	{
		MobileExcludedMappingContexts.Add(MouseLookIMC.Object);
	}

	// 软引用晚绑：IA_Interact 是脚本生成的资产，构造函数只在模块加载时跑一次，
	// 用 ConstructorHelpers 的话新建出来的资产在同一次会话里永远解析不到。
	InteractAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Interact.IA_Interact")));
	InteractKey = EKeys::F;
}

namespace
{
	bool ContextMapsAction(const UInputMappingContext* Context, const UInputAction* Action)
	{
		if (!Context || !Action)
		{
			return false;
		}
		for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
		{
			if (Mapping.Action == Action)
			{
				return true;
			}
		}
		return false;
	}
}

void ADeliveryPlayerController::EnsureInteractMapping(UEnhancedInputLocalPlayerSubsystem* Subsystem)
{
	UInputAction* Interact = InteractAction.LoadSynchronous();
	if (!Subsystem || !Interact || !InteractKey.IsValid())
	{
		return;
	}

	// 资产里已经有映射就什么都不做——重复映射同一个动作没必要，也省得两套绑定互相干扰。
	for (const UInputMappingContext* Context : DefaultMappingContexts)
	{
		if (ContextMapsAction(Context, Interact))
		{
			return;
		}
	}
	for (const UInputMappingContext* Context : MobileExcludedMappingContexts)
	{
		if (ContextMapsAction(Context, Interact))
		{
			return;
		}
	}

	if (!RuntimeInteractContext)
	{
		RuntimeInteractContext = NewObject<UInputMappingContext>(this, TEXT("RuntimeInteractContext"));
		RuntimeInteractContext->MapKey(Interact, InteractKey);
	}
	Subsystem->AddMappingContext(RuntimeInteractContext, 0);
	UE_LOG(LogDelivery, Log,
		TEXT("IMC 资产里没有 %s 的映射，已在运行时补上 %s 键。"
			 "（正常情况下它应该在 IMC_Default 里，检查那个资产是不是被拉取冲掉了）"),
		*GetNameSafe(Interact), *InteractKey.ToString());
}

void ADeliveryPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!IsLocalPlayerController())
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
		{
			Subsystem->AddMappingContext(CurrentContext, 0);
		}

		for (UInputMappingContext* CurrentContext : MobileExcludedMappingContexts)
		{
			Subsystem->AddMappingContext(CurrentContext, 0);
		}

		EnsureInteractMapping(Subsystem);
	}
}

void ADeliveryPlayerController::RequestAnswerCall()
{
	if (HasAuthority())
	{
		if (UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(this))
		{
			Phone->AnswerCurrentCall();
		}

		return;
	}

	ServerAnswerCall();
}

void ADeliveryPlayerController::ServerAnswerCall_Implementation()
{
	RequestAnswerCall();
}

void ADeliveryPlayerController::RequestHangUpCall()
{
	if (HasAuthority())
	{
		if (UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(this))
		{
			Phone->HangUpCurrentCall();
		}

		return;
	}

	ServerHangUpCall();
}

void ADeliveryPlayerController::ServerHangUpCall_Implementation()
{
	RequestHangUpCall();
}
