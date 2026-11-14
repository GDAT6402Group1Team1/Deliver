#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "AI/Decision/DeliveryNpcDecisionTrace.h"
#include "DeliveryAiInspectorWidget.generated.h"

class UButton;
class USizeBox;
class UScrollBox;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;

DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryAiInspectorExpandedChanged, bool);
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryAiInspectorHistorySelected, int32);
DECLARE_MULTICAST_DELEGATE(FDeliveryAiInspectorLatestRequested);

// 只显示处理器提供的真实快照，控件树由 WBP 保存，折叠和选页属于本机表现
UCLASS()
class DELIVERY_API UDeliveryAiInspectorWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetTrace(const FDeliveryNpcDecisionTrace& Value, bool bHistorical = false);
    void SetHistory(const TArray<FDeliveryNpcDecisionTrace>& Values);
    void ClearTrace();
    void SetAvailableSize(FVector2D MaxSize);
    void SetExpanded(bool bValue);
    UFUNCTION() void ToggleExpanded();
    bool IsExpanded() const { return bExpanded; }

    FDeliveryAiInspectorExpandedChanged OnExpandedChanged;
    FDeliveryAiInspectorHistorySelected OnHistorySelected;
    FDeliveryAiInspectorLatestRequested OnLatestRequested;

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UPROPERTY(meta=(BindWidget)) TObjectPtr<USizeBox> InspectorSize;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<USizeBox> DetailsArea;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UVerticalBox> ExpandedPanel;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UVerticalBox> CompactSummary;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> EmptyLabel;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> NpcName;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> StageLabel;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> CompactProposed;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> CompactExecution;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> HistoryBanner;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UButton> LatestButton;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UButton> ExpandButton;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> ExpandLabel;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UWidgetSwitcher> TabPages;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UScrollBox> DetailsScroll;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> OverviewText;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> PerceptionText;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> ActionText;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> TimingText;
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> HistoryEmptyLabel;

private:
    UFUNCTION() void RequestLatest();
    UFUNCTION() void ShowOverview();
    UFUNCTION() void ShowPerception();
    UFUNCTION() void ShowActions();
    UFUNCTION() void ShowTiming();
    UFUNCTION() void ShowHistory();
    UFUNCTION() void SelectHistory0();
    UFUNCTION() void SelectHistory1();
    UFUNCTION() void SelectHistory2();
    UFUNCTION() void SelectHistory3();
    UFUNCTION() void SelectHistory4();
    UFUNCTION() void SelectHistory5();
    UFUNCTION() void SelectHistory6();
    UFUNCTION() void SelectHistory7();
    void SelectTab(int32 Index);
    void SelectHistory(int32 Index);
    void BindButtons(bool bBind);
    void RefreshTrace();
    void RefreshHistory();
    void RefreshPresentation();
    void ApplySize();

    FDeliveryNpcDecisionTrace Trace;
    TArray<FDeliveryNpcDecisionTrace> History;
    FVector2D AvailableSize = FVector2D(460, 540);
    int32 ActiveTab = 0;
    bool bHasTrace = false;
    bool bHistoricalTrace = false;
    bool bExpanded = false;
};
