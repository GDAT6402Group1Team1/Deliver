#if !UE_BUILD_SHIPPING
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Delivery.h"

// 在 PIE 中经过真实按键映射测试输入，不直接调用玩法函数
static FAutoConsoleCommandWithWorldAndArgs DeliveryInputKeyCommand(
    TEXT("Delivery.Input.Key"), TEXT("PIE 输入检查：Delivery.Input.Key R down|up"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
    {
        if (!World || World->WorldType != EWorldType::PIE || Args.Num() != 2) return;
        APlayerController* Player = World->GetFirstPlayerController();
        const FKey Key(*Args[0]);
        if (!Player || !Player->IsLocalController() || !Key.IsValid()) return;
        const bool bDown = Args[1] == TEXT("down");
        if (!bDown && Args[1] != TEXT("up")) return;
        Player->InputKey(FInputKeyEventArgs(nullptr, FInputDeviceId::CreateFromInternalId(0), Key,
            bDown ? IE_Pressed : IE_Released, bDown ? 1.0f : 0.0f, false, FPlatformTime::Cycles64()));
        UE_LOG(LogDelivery, Log, TEXT("[InputTest] %s %s %s"), *World->GetName(), *Args[0], *Args[1]);
    }));
#endif
