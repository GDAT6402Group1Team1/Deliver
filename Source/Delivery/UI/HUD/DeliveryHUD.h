#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Game/DeliveryStartupFlow.h"
#include "DeliveryHUD.generated.h"

class ADeliveryCharacter;
class ADeliveryNpcCharacter;
class UDeliveryConversationWidget;
class UDeliveryHotbarWidget;
class UDeliveryInventoryComponent;
class UDeliveryPhoneController;
class UDeliveryStartupWidget;
class UDeliveryTrunkStatusWidget;
class UDeliverySpeechInputComponent;
class UDeliverySpeechStatusWidget;
class UDeliveryAiInspectorWidget;
class UDeliveryAiInspectorController;
class UDeliveryMinimapWidget;
enum class EDeliverySpeechInputState : uint8;

// 管理本机界面处理器、面板开关和鼠标模式，控件负责布局和显示
UCLASS()
class DELIVERY_API ADeliveryHUD : public AHUD
{
    GENERATED_BODY()
public:
    ADeliveryHUD();
    void OpenResidentConversation(ADeliveryNpcCharacter* Resident);
    void CloseResidentConversation();
    void ToggleAiInspector();
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
    bool HasSpeechStatusWidget() const;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    UPROPERTY(EditDefaultsOnly, Category="UI|Hotbar")
    TSoftClassPtr<UDeliveryHotbarWidget> HotbarWidgetClass;
    UPROPERTY(EditDefaultsOnly, Category="UI|Conversation")
    TSoftClassPtr<UDeliveryConversationWidget> ConversationWidgetClass;
    UPROPERTY(EditDefaultsOnly, Category="UI|Speech")
    TSoftClassPtr<UDeliverySpeechStatusWidget> SpeechStatusWidgetClass;
    UPROPERTY(EditDefaultsOnly, Category="UI|AI Inspector")
    TSoftClassPtr<UDeliveryAiInspectorWidget> AiInspectorWidgetClass;
private:
    void InitializeAiInspector();
    void RefreshAiInspectorLayout();
    UPROPERTY(Transient) TObjectPtr<UDeliveryAiInspectorWidget> AiInspectorWidget;
    UPROPERTY(Transient) TObjectPtr<UDeliveryAiInspectorController> AiInspectorController;
    TWeakObjectPtr<UDeliveryMinimapWidget> InspectorMinimap;
    double NextInspectorMinimapSearchAt = 0;
    void InitializeSpeechStatus();
    void RefreshSpeechStatusLayout();
    void HandleSpeechStateChanged(EDeliverySpeechInputState State);
    UPROPERTY(Transient) TObjectPtr<UDeliverySpeechStatusWidget> SpeechStatusWidget;
    TWeakObjectPtr<UDeliverySpeechInputComponent> BoundSpeechInput;
    FDelegateHandle SpeechStateHandle;
    void RefreshConversationLayout();
    UPROPERTY(Transient) TObjectPtr<UDeliveryConversationWidget> ConversationWidget;
    void InitializeStartupMenu();
    UFUNCTION()
    void HandleStartupPhase(EDeliveryStartupPhase Phase);
    void CloseStartupMenu();
    FTimerHandle StartupCloseTimer;
    TWeakObjectPtr<ADeliveryStartupFlow> StartupFlow;
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryStartupWidget> StartupWidget;
    void ApplyInputMode();
    void TryApplyInputMode();
    bool CanApplyInputMode() const;
    // 后台窗口只记住最新模式，重新获得本视口焦点后应用一次
    bool bInputModePending = false;
    bool bEndingPlay = false;
    // 多个面板同时打开时，关闭其中一个不能收起鼠标
    TSet<FName> OpenPanels;
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryPhoneController> PhoneController;
    void RefreshHotbar();
    void RefreshTrunkStatus();
    UPROPERTY(Transient) TObjectPtr<UDeliveryTrunkStatusWidget> TrunkStatus;
    void RemoveHotbar();
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryHotbarWidget> HotbarWidget;
    TWeakObjectPtr<UDeliveryInventoryComponent> BoundInventory;
    TWeakObjectPtr<ADeliveryCharacter> BoundCharacter;
};
