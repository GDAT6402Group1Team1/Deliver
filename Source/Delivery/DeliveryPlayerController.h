// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "DeliveryPlayerController.generated.h"

class UInputAction;
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

	/**
	 * 交互键（F）。上面那些 IMC 资产里没有映射它时，运行时补一份。
	 *
	 * 为什么要兜底：F→IA_Interact 这条映射存在 IMC_Default.uasset 里，
	 * 而这个二进制资产被 git 拉取冲掉过两次。症状极具迷惑性——浮窗「按 F 驾驶」
	 * 照常显示（浮窗不依赖按键绑定），只有按下去没反应，很容易当成代码坏了去查编译。
	 * 补一份运行时映射之后，功能就不再取决于那个资产有没有被正确提交。
	 */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TSoftObjectPtr<UInputAction> InteractAction;

	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	FKey InteractKey;

	virtual void SetupInputComponent() override;

private:

	/** 已有 IMC 里都没映射交互键时，建一份临时的补上。 */
	void EnsureInteractMapping(class UEnhancedInputLocalPlayerSubsystem* Subsystem);

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> RuntimeInteractContext;
};
