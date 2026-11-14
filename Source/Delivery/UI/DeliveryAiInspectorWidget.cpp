#include "UI/DeliveryAiInspectorWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/WidgetSwitcher.h"

namespace
{
const TCHAR* TabNames[] = { TEXT("OverviewTab"), TEXT("PerceptionTab"), TEXT("ActionsTab"), TEXT("TimingTab"), TEXT("HistoryTab") };

FString Provided(const FString& Value, int32 Limit = 512)
{
    FString Text = Value.TrimStartAndEnd();
    return Text.IsEmpty() ? TEXT("未提供") : Text.Left(Limit);
}

FString StageText(FName Stage)
{
    if (Stage == TEXT("Queued")) return TEXT("等待模型");
    if (Stage == TEXT("Requesting")) return TEXT("模型处理中");
    if (Stage == TEXT("Decision")) return TEXT("建议已返回");
    if (Stage == TEXT("ModelDone")) return TEXT("模型响应结束");
    if (Stage == TEXT("ReplyDone")) return TEXT("回复传输结束");
    if (Stage == TEXT("ActionStarted")) return TEXT("动作已开始");
    if (Stage == TEXT("ActionFinished")) return TEXT("动作已结束");
    if (Stage == TEXT("Failed")) return TEXT("本轮失败");
    if (Stage == TEXT("Cancelled")) return TEXT("本轮已取消");
    if (Stage == TEXT("Unavailable")) return TEXT("对象暂不可观察");
    return TEXT("尚无本轮记录");
}

FString ReplyStageText(FName Stage)
{
    if (Stage == TEXT("Received")) return TEXT("收到回复文字");
    if (Stage == TEXT("TransferDone")) return TEXT("回复传输结束");
    if (Stage == TEXT("Finished")) return TEXT("回复已结束");
    if (Stage == TEXT("Cancelled")) return TEXT("回复已取消");
    if (Stage == TEXT("Failed")) return TEXT("回复失败");
    return TEXT("未提供");
}

FString Milliseconds(double Value)
{
    if (!FMath::IsFinite(Value) || Value < 0) return TEXT("未提供");
    return Value < 1000 ? FString::Printf(TEXT("%.0f 毫秒"), Value)
        : FString::Printf(TEXT("%.2f 秒"), Value / 1000);
}

FString Count(int32 Value)
{
    return Value < 0 ? TEXT("未提供") : FString::FromInt(Value);
}

FString ActionLabel(const FString& Value)
{
    // 只翻译协议动作名的完整前缀，不替换玩家原话或模型台词中的词语
    struct FActionLabel { const TCHAR* Code; const TCHAR* Label; };
    const FActionLabel Labels[] = {
        { TEXT("ContinueCurrent"), TEXT("继续当前活动") }, { TEXT("Wait"), TEXT("等待") },
        { TEXT("LookAt"), TEXT("看向目标") }, { TEXT("MoveTo"), TEXT("走向目标") },
        { TEXT("StrikeOnce"), TEXT("挥拳一次") }, { TEXT("HoldVehicle"), TEXT("拽住车辆") },
        { TEXT("ReleaseVehicle"), TEXT("松开车辆") }, { TEXT("LieDown"), TEXT("躺下") },
        { TEXT("GetUp"), TEXT("起身") }, { TEXT("Pace"), TEXT("来回踱步") },
        { TEXT("MoveAside"), TEXT("让到一旁") }, { TEXT("Fight"), TEXT("打架") },
        { TEXT("Flee"), TEXT("逃离") }, { TEXT("Follow"), TEXT("跟随主人") },
        { TEXT("Stay"), TEXT("原地等候") }, { TEXT("CancelCompanionTask"), TEXT("取消当前委托") },
        { TEXT("FetchPackage"), TEXT("前往取件") }, { TEXT("TakePackage"), TEXT("接过包裹") },
        { TEXT("DeliverPackage"), TEXT("递送包裹") }, { TEXT("ReturnPackage"), TEXT("归还包裹") },
        { TEXT("OfferCaterpillar"), TEXT("递出小绿") }, { TEXT("GoHome"), TEXT("回家") },
        { TEXT("HomeAction"), TEXT("居家活动") }, { TEXT("None"), TEXT("未选择动作") }
    };
    const FString Text = Value.TrimStartAndEnd();
    for (const auto& Entry : Labels)
    {
        const FString Code(Entry.Code);
        if (Text == Code) return Entry.Label;
        if (Text.StartsWith(Code + TEXT(" ")) || Text.StartsWith(Code + TEXT("；")))
            return FString(Entry.Label) + Text.Mid(Code.Len());
    }
    return Provided(Text);
}

FString Lines(const TArray<FString>& Values, int32 Limit, bool bActions = false)
{
    FString Result;
    for (int32 Index = 0; Index < FMath::Min(Values.Num(), Limit); ++Index)
    {
        if (Values[Index].TrimStartAndEnd().IsEmpty()) continue;
        if (!Result.IsEmpty()) Result += TEXT("\n");
        Result += TEXT("• ") + (bActions ? ActionLabel(Values[Index]) : Values[Index]).Left(160);
    }
    return Result.IsEmpty() ? TEXT("未提供") : Result;
}

// 面板没有实时计时器，耗时只在收到带实测数值的快照后变化
void SetLabel(UTextBlock* Label, const FString& Value)
{
    if (Label && Label->GetText().ToString() != Value) Label->SetText(FText::FromString(Value));
}

bool SameRevision(const FDeliveryNpcDecisionTrace& First, const FDeliveryNpcDecisionTrace& Second)
{
    return First.WorldEpoch == Second.WorldEpoch && First.RequestId == Second.RequestId &&
        First.Revision == Second.Revision && First.NpcName == Second.NpcName && First.Stage == Second.Stage;
}
}

void UDeliveryAiInspectorWidget::NativeConstruct()
{
    Super::NativeConstruct();
    BindButtons(true);
    // HUD 可以先传快照再加入视口，构建时补一次显示即可
    RefreshTrace();
    RefreshHistory();
    RefreshPresentation();
}

void UDeliveryAiInspectorWidget::NativeDestruct()
{
    BindButtons(false);
    Super::NativeDestruct();
}

void UDeliveryAiInspectorWidget::SetTrace(const FDeliveryNpcDecisionTrace& Value, bool bHistorical)
{
    const bool bHasValue = Value.RequestId.IsValid() || Value.Revision > 0;
    if (bHasTrace == bHasValue && bHistoricalTrace == bHistorical && SameRevision(Trace, Value)) return;
    Trace = Value;
    bHasTrace = bHasValue;
    bHistoricalTrace = bHistorical;
    RefreshTrace();
    RefreshPresentation();
}

void UDeliveryAiInspectorWidget::ClearTrace()
{
    Trace = {};
    bHasTrace = false;
    bHistoricalTrace = false;
    RefreshTrace();
    RefreshPresentation();
}

void UDeliveryAiInspectorWidget::SetHistory(const TArray<FDeliveryNpcDecisionTrace>& Values)
{
    const int32 NewCount = FMath::Min(Values.Num(), 8);
    bool bChanged = History.Num() != NewCount;
    for (int32 Index = 0; !bChanged && Index < NewCount; ++Index)
        bChanged = !SameRevision(History[Index], Values[Index]);
    if (!bChanged) return;
    History.Reset(NewCount);
    for (int32 Index = 0; Index < NewCount; ++Index) History.Add(Values[Index]);
    RefreshHistory();
}

void UDeliveryAiInspectorWidget::SetAvailableSize(FVector2D MaxSize)
{
    if (!FMath::IsFinite(MaxSize.X) || !FMath::IsFinite(MaxSize.Y)) return;
    MaxSize.X = FMath::Max(1.0, MaxSize.X);
    MaxSize.Y = FMath::Max(1.0, MaxSize.Y);
    if (AvailableSize.Equals(MaxSize, .5)) return;
    AvailableSize = MaxSize;
    ApplySize();
}

void UDeliveryAiInspectorWidget::ApplySize()
{
    const float Width = FMath::Min(bExpanded ? 460.f : 330.f, float(AvailableSize.X));
    const float Height = FMath::Min(540.f, float(AvailableSize.Y));
    if (InspectorSize)
    {
        InspectorSize->SetWidthOverride(Width);
        InspectorSize->SetMaxDesiredHeight(Height);
        if (bExpanded) InspectorSize->SetHeightOverride(Height);
        else InspectorSize->ClearHeightOverride();
    }
    // 详情在 WBP 的 Fill 槽中使用标题与标签之后的剩余空间，不估算文字高度
    if (DetailsArea) DetailsArea->ClearHeightOverride();
}

void UDeliveryAiInspectorWidget::RefreshTrace()
{
    SetLabel(NpcName, Trace.NpcName.IsEmpty() ? TEXT("尚未选择人物") : Trace.NpcName.Left(48));
    SetLabel(StageLabel, !Trace.Stage.IsNone() ? StageText(Trace.Stage) : TEXT("暂无记录"));
    SetLabel(CompactProposed, TEXT("模型建议  ") + ActionLabel(Trace.ProposedAction).Left(96));
    SetLabel(CompactExecution, TEXT("执行结果  ") + Provided(Trace.ExecutionResult, 96));
    const FString EmptyText = Trace.Stage == TEXT("Unavailable") ? TEXT("对象暂不可观察") : TEXT("尚无本轮记录");
    SetLabel(EmptyLabel, EmptyText);
    SetLabel(HistoryBanner, TEXT("历史快照 · 不随当前状态更新"));
    if (!bHasTrace)
    {
        for (auto* Label : { OverviewText.Get(), PerceptionText.Get(), ActionText.Get(), TimingText.Get() })
            SetLabel(Label, EmptyText);
        return;
    }
    FString Overview = TEXT("当前活动\n") + ActionLabel(Trace.CurrentActivity) +
        TEXT("\n\n模型建议\n") + ActionLabel(Trace.ProposedAction) +
        TEXT("\n\n建议目标\n") + Provided(Trace.ProposedTarget) +
        TEXT("\n\n游戏确认\n") + ActionLabel(Trace.ValidatedAction) +
        TEXT("\n\n执行结果\n") + Provided(Trace.ExecutionResult) +
        TEXT("\n\n回复摘要\n") + Provided(Trace.ReplyPreview);
    if (!Trace.RejectionReason.IsEmpty()) Overview += TEXT("\n\n未执行原因\n") + Provided(Trace.RejectionReason);
    SetLabel(OverviewText, Overview);
    SetLabel(PerceptionText, TEXT("游戏记录的观察（发给后端）\n") + Lines(Trace.Observations, 3) +
        TEXT("\n坐标与时间来自游戏记录；后端可能压缩或省略这些信息") +
        TEXT("\n\n玩家原话\n") + Provided(Trace.PlayerText) +
        TEXT("\n\n输入摘要\n") + Provided(Trace.InputSummary) +
        TEXT("\n\n现场对象\n") + Lines(Trace.Subjects, 6) +
        TEXT("\n\n游戏请求的背景摘要\n") + Lines(Trace.Memory, 4));
    FString Actions = TEXT("本轮候选\n") + Lines(Trace.Candidates, 12, true) +
        TEXT("\n\n模型建议\n") + ActionLabel(Trace.ProposedAction) +
        TEXT("\n\n建议目标\n") + Provided(Trace.ProposedTarget) +
        TEXT("\n\n游戏确认\n") + ActionLabel(Trace.ValidatedAction) +
        TEXT("\n\n执行结果\n") + Provided(Trace.ExecutionResult);
    if (!Trace.RejectionReason.IsEmpty()) Actions += TEXT("\n\n未执行原因\n") + Provided(Trace.RejectionReason);
    SetLabel(ActionText, Actions);
    const FString RequestDigits=Trace.RequestId.ToString(EGuidFormats::Digits);
    FString Timing = TEXT("本轮记录\n") + (Trace.RequestId.IsValid() ? RequestDigits.Left(16)+TEXT("\n")+RequestDigits.Mid(16) : TEXT("未提供")) +
        TEXT("\n\n实际模型\n") + Provided(Trace.ActualModelName, 96) +
        TEXT("\n\n模型提供方\n") + Provided(Trace.ModelProvider, 48) +
        TEXT("\n\n请求来源 / 处理方式\n") + Provided(Trace.RequestSource, 96) + TEXT(" / ") + Provided(Trace.Route, 96) +
        TEXT("\n\n回复事件\n") + ReplyStageText(Trace.ReplyStage) +
        TEXT("\n\n首句文字\n") + Provided(Trace.FirstReplyText) +
        TEXT("\n\n事件耗时\n发出请求 → 收到建议  ") + Milliseconds(Trace.DecisionElapsedMs) +
        TEXT("\n入队 → 响应传输结束  ") + Milliseconds(Trace.TotalElapsedMs) +
        TEXT("\n后端排队  ") + Milliseconds(Trace.BackendQueueMs) +
        TEXT("\n后端决策（含准备）  ") + Milliseconds(Trace.BackendDecisionMs) +
        TEXT("\n后端总处理  ") + Milliseconds(Trace.BackendElapsedMs) +
        TEXT("\n首句文本通过检查  ") + Milliseconds(Trace.FirstSentenceMs) +
        TEXT("\n\n模型用量\n输入 token  ") + Count(Trace.InputTokens) +
        TEXT("\n命中缓存 token  ") + Count(Trace.CachedInputTokens) +
        TEXT("\n输出 token  ") + Count(Trace.OutputTokens) +
        TEXT("\n\n以上耗时来自已发生事件，不代表语音播放总时长");
    if (!Trace.FallbackReason.IsEmpty()) Timing += TEXT("\n\n处理方式变化原因\n") + Provided(Trace.FallbackReason);
    SetLabel(TimingText, Timing);
}

void UDeliveryAiInspectorWidget::RefreshHistory()
{
    if (!WidgetTree) return;
    if (HistoryEmptyLabel) HistoryEmptyLabel->SetVisibility(History.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
    for (int32 Index = 0; Index < 8; ++Index)
    {
        auto* Button = Cast<UButton>(WidgetTree->FindWidget(*FString::Printf(TEXT("HistoryButton%d"), Index)));
        auto* Label = Cast<UTextBlock>(WidgetTree->FindWidget(*FString::Printf(TEXT("HistoryText%d"), Index)));
        const bool bPresent = History.IsValidIndex(Index);
        if (Button) Button->SetVisibility(bPresent ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
        if (!bPresent) { SetLabel(Label, TEXT("")); continue; }
        const auto& Item = History[Index];
        SetLabel(Label, Provided(Item.NpcName, 32) + TEXT(" · ") + StageText(Item.Stage) +
            TEXT("\n") + ActionLabel(Item.ProposedAction).Left(70) + TEXT(" · ") + Provided(Item.ExecutionResult, 70));
    }
}

void UDeliveryAiInspectorWidget::RefreshPresentation()
{
    if (ExpandedPanel) ExpandedPanel->SetVisibility(bExpanded ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    if (CompactSummary) CompactSummary->SetVisibility(!bExpanded && bHasTrace ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
    if (EmptyLabel) EmptyLabel->SetVisibility(!bExpanded && !bHasTrace ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
    if (HistoryBanner) HistoryBanner->SetVisibility(bHistoricalTrace ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
    if (LatestButton) LatestButton->SetVisibility(bHistoricalTrace ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    SetLabel(ExpandLabel, bExpanded ? TEXT("F6 收起") : TEXT("F6 展开"));
    if (ExpandButton) ExpandButton->SetToolTipText(FText::FromString(TEXT("LLM NPC OBESERVATION")));
    if (TabPages) TabPages->SetActiveWidgetIndex(ActiveTab);
    if (WidgetTree)
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(TabNames); ++Index)
            if (auto* Button = Cast<UButton>(WidgetTree->FindWidget(TabNames[Index]))) Button->SetIsEnabled(Index != ActiveTab);
    ApplySize();
}

void UDeliveryAiInspectorWidget::ToggleExpanded()
{
    SetExpanded(!bExpanded);
}

void UDeliveryAiInspectorWidget::SetExpanded(bool bValue)
{
    if (bExpanded == bValue) return;
    bExpanded = bValue;
    RefreshPresentation();
    OnExpandedChanged.Broadcast(bExpanded);
}

void UDeliveryAiInspectorWidget::RequestLatest() { OnLatestRequested.Broadcast(); }
void UDeliveryAiInspectorWidget::SelectTab(int32 Index)
{
    if (Index < 0 || Index >= UE_ARRAY_COUNT(TabNames) || ActiveTab == Index) return;
    ActiveTab = Index;
    RefreshPresentation();
    if (DetailsScroll) DetailsScroll->ScrollToStart();
}
void UDeliveryAiInspectorWidget::ShowOverview() { SelectTab(0); }
void UDeliveryAiInspectorWidget::ShowPerception() { SelectTab(1); }
void UDeliveryAiInspectorWidget::ShowActions() { SelectTab(2); }
void UDeliveryAiInspectorWidget::ShowTiming() { SelectTab(3); }
void UDeliveryAiInspectorWidget::ShowHistory() { SelectTab(4); }
void UDeliveryAiInspectorWidget::SelectHistory(int32 Index)
{
    if (History.IsValidIndex(Index)) OnHistorySelected.Broadcast(Index);
}
void UDeliveryAiInspectorWidget::SelectHistory0() { SelectHistory(0); }
void UDeliveryAiInspectorWidget::SelectHistory1() { SelectHistory(1); }
void UDeliveryAiInspectorWidget::SelectHistory2() { SelectHistory(2); }
void UDeliveryAiInspectorWidget::SelectHistory3() { SelectHistory(3); }
void UDeliveryAiInspectorWidget::SelectHistory4() { SelectHistory(4); }
void UDeliveryAiInspectorWidget::SelectHistory5() { SelectHistory(5); }
void UDeliveryAiInspectorWidget::SelectHistory6() { SelectHistory(6); }
void UDeliveryAiInspectorWidget::SelectHistory7() { SelectHistory(7); }

void UDeliveryAiInspectorWidget::BindButtons(bool bBind)
{
    if (!WidgetTree) return;
    auto Bind = [this, bBind](UButton* Button, FName Function)
    {
        if (!Button) return;
        FScriptDelegate Callback;
        Callback.BindUFunction(this, Function);
        if (bBind) Button->OnClicked.AddUnique(Callback);
        else Button->OnClicked.Remove(Callback);
    };
    Bind(ExpandButton, GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, ToggleExpanded));
    Bind(LatestButton, GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, RequestLatest));
    const FName Tabs[] = { GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, ShowOverview),
        GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, ShowPerception), GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, ShowActions),
        GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, ShowTiming), GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, ShowHistory) };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(TabNames); ++Index)
        Bind(Cast<UButton>(WidgetTree->FindWidget(TabNames[Index])), Tabs[Index]);
    const FName HistoryHandlers[] = { GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, SelectHistory0),
        GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, SelectHistory1), GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, SelectHistory2),
        GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, SelectHistory3), GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, SelectHistory4),
        GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, SelectHistory5), GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, SelectHistory6),
        GET_FUNCTION_NAME_CHECKED(UDeliveryAiInspectorWidget, SelectHistory7) };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(HistoryHandlers); ++Index)
        Bind(Cast<UButton>(WidgetTree->FindWidget(*FString::Printf(TEXT("HistoryButton%d"), Index))), HistoryHandlers[Index]);
}
