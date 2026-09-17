// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "DeliveryPlayerController.generated.h"

class UInputMappingContext;

/**
 * PlayerController that only installs Enhanced Input mapping contexts.
 */
UCLASS()
class ADeliveryPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	ADeliveryPlayerController();

	/**
	 * 接听当前来电。客户端调用会自动转成 Server RPC。
	 *
	 * 电话队列挂在 GameState 上，而 GameState 不属于任何客户端，
	 * 客户端没法直接对它发 RPC——所以玩家的接听/挂断意图统一从这里中转。
	 */
	UFUNCTION(BlueprintCallable, Category="Phone")
	void RequestAnswerCall();

	/** 挂断。响铃时是拒接，通话中是提前结束。两种都会影响所有玩家。 */
	UFUNCTION(BlueprintCallable, Category="Phone")
	void RequestHangUpCall();

protected:

	UFUNCTION(Server, Reliable)
	void ServerAnswerCall();

	UFUNCTION(Server, Reliable)
	void ServerHangUpCall();


	/** Input Mapping Contexts always applied */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/** Extra mapping contexts for mouse look, skipped on touch devices */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> MobileExcludedMappingContexts;

	virtual void SetupInputComponent() override;
};
