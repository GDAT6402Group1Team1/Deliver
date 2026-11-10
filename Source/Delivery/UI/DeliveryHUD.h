#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Game/DeliveryStartupFlow.h"
#include "DeliveryHUD.generated.h"

class ADeliveryCharacter;
class UDeliveryHotbarWidget;
class UDeliveryInventoryComponent;
class UDeliveryPhoneController;
class UDeliveryStartupWidget;

// 管理本机界面处理器、面板开关和鼠标模式，控件负责布局和显示
UCLASS()
class DELIVERY_API ADeliveryHUD : public AHUD
{
    GENERATED_BODY()
public:
    ADeliveryHUD();
    virtual void Tick(float DeltaSeconds) override;
    // 骑车时读取驾驶员的背包，步行时读取当前角色的背包
    static ADeliveryCharacter* ResolveCharacter(APawn* Pawn);
    UFUNCTION(BlueprintPure, Category="UI")
    UDeliveryPhoneController* GetPhoneController();
    void SetPanelOpen(FName PanelId, bool bOpen);
    bool IsPanelOpen(FName PanelId) const { return OpenPanels.Contains(PanelId); }
    // 诊断命令只读取面板状态，不修改界面
    const TSet<FName>& GetOpenPanelsForDebug() const { return OpenPanels; }
    bool IsAnyPanelOpen() const { return !OpenPanels.IsEmpty(); }
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    UPROPERTY(EditDefaultsOnly, Category="UI|Hotbar")
    TSoftClassPtr<UDeliveryHotbarWidget> HotbarWidgetClass;
private:
    void InitializeStartupMenu();
    UFUNCTION()
    void HandleStartupPhase(EDeliveryStartupPhase Phase);
    void CloseStartupMenu();
    FTimerHandle StartupCloseTimer;
    TWeakObjectPtr<ADeliveryStartupFlow> StartupFlow;
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryStartupWidget> StartupWidget;
    void ApplyInputMode();
    // 多个面板同时打开时，关闭其中一个不能收起鼠标
    TSet<FName> OpenPanels;
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryPhoneController> PhoneController;
    void RefreshHotbar();
    void RemoveHotbar();
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryHotbarWidget> HotbarWidget;
    TWeakObjectPtr<UDeliveryInventoryComponent> BoundInventory;
    TWeakObjectPtr<ADeliveryCharacter> BoundCharacter;
};
