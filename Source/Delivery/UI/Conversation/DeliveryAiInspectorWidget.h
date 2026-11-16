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

/// 面板展开或收起时广播，参数为 true 表示展开
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryAiInspectorExpandedChanged, bool);

/// 玩家点了第几条历史记录（0 到 7）
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryAiInspectorHistorySelected, int32);

/// 玩家点了回到最新按钮 LatestButton
DECLARE_MULTICAST_DELEGATE(FDeliveryAiInspectorLatestRequested);

/**
 * @brief AI 观察面板 WBP 的父类，按 F6 展开或收起，显示居民这一轮决策的记录
 *
 * UDeliveryAiInspectorController 调用 SetTrace、SetHistory 交来快照，面板只显示，不读游戏状态
 * 控件树存在 WBP 里；展开状态和当前分页只在本机记着，不同步
 * 分页依次是概况、观察、动作、耗时、历史，按钮名字是 TabNames 里的那几个
 */
UCLASS()
class DELIVERY_API UDeliveryAiInspectorWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    // ── 数据 ──

    /// @brief 显示一条决策记录；和正在显示的同一轮同一阶段时直接返回
    /// @param bHistorical true 表示这是玩家点开的历史记录，面板会显示历史横幅和回到最新按钮
    void SetTrace(const FDeliveryNpcDecisionTrace& Value, bool bHistorical = false);

    /// @brief 更新历史分页，最多显示前 8 条；内容没变时直接返回
    void SetHistory(const TArray<FDeliveryNpcDecisionTrace>& Values);

    /// @brief 清空正在显示的记录
    void ClearTrace();

    // ── 布局 ──

    /// @brief HUD 告诉面板最多能占多大，面板宽高不超过它
    void SetAvailableSize(FVector2D MaxSize);

    /// @brief 展开或收起面板，变化时广播 OnExpandedChanged
    void SetExpanded(bool bValue);

    /// @brief 切换展开和收起，ExpandButton 和 F6 都调用它
    UFUNCTION() void ToggleExpanded();

    /// @brief 面板是否展开
    bool IsExpanded() const { return bExpanded; }

    // ── 事件 ──
    FDeliveryAiInspectorExpandedChanged OnExpandedChanged;
    FDeliveryAiInspectorHistorySelected OnHistorySelected;
    FDeliveryAiInspectorLatestRequested OnLatestRequested;

protected:
    // ── 生命周期 ──
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    // ── 控件 ──
    // 都在 WBP 里，名字必须一致；收起时只显示 CompactSummary（一行建议和一行执行结果），展开时显示 ExpandedPanel 和分页 TabPages
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
    // ── 按钮回调 ──
    // BindButtons 把这些函数绑到各按钮的 OnClicked 上；按钮回调不能带参数，所以每个分页和每条历史各有一个函数

    /// @brief 回到最新按钮：广播 OnLatestRequested
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

    /// @brief 切到第 Index 个分页，并把详情滚回顶部
    void SelectTab(int32 Index);

    /// @brief 第 Index 条历史存在时广播 OnHistorySelected
    void SelectHistory(int32 Index);

    /// @brief bBind 为 true 时把所有按钮的点击绑到上面的回调，false 时解绑
    void BindButtons(bool bBind);

    // ── 刷新 ──

    /// @brief 把 Trace 写进各分页的文字控件
    void RefreshTrace();

    /// @brief 把 History 写进 HistoryButton0 到 7，没有记录的按钮隐藏
    void RefreshHistory();

    /// @brief 按 bExpanded、bHasTrace、bHistoricalTrace 切换各区域的显示，更新分页按钮，再调用 ApplySize
    void RefreshPresentation();

    /// @brief 按展开状态和 AvailableSize 设置面板宽高：收起时宽 330，展开时宽 460，高不超过 540
    void ApplySize();

    // ── 状态 ──

    /// 正在显示的记录
    FDeliveryNpcDecisionTrace Trace;

    /// 历史记录，最多 8 条
    TArray<FDeliveryNpcDecisionTrace> History;

    /// HUD 给的最大可用尺寸
    FVector2D AvailableSize = FVector2D(460, 540);

    /// 当前分页：0 概况、1 观察、2 动作、3 耗时、4 历史
    int32 ActiveTab = 0;

    /// 有没有记录可以显示
    bool bHasTrace = false;

    /// 正在显示的是不是玩家点开的历史记录
    bool bHistoricalTrace = false;

    /// 面板是否展开
    bool bExpanded = false;
};
