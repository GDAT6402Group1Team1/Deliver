#include "DeliveryAiInspectorAssetTools.h"

#include "UI/DeliveryAiInspectorWidget.h"
#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WrapBox.h"
#include "Editor.h"
#include "Engine/Font.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace
{
const FString Folder = TEXT("/Game/UI/Conversation");
const FString BlueprintName = TEXT("WBP_AiInspector");

FLinearColor Color(uint8 R, uint8 G, uint8 B, uint8 A = 255)
{
    return FLinearColor::FromSRGBColor(FColor(R, G, B, A));
}

// 根据蓝灰效果图统一面板层次，颜色、间距与字体保存到 WBP 后可直接调整
const FLinearColor PanelColor = Color(28, 42, 56, 242);
const FLinearColor InsetColor = Color(34, 50, 66, 250);
const FLinearColor BorderColor = Color(101, 136, 161);
const FLinearColor AccentColor = Color(160, 217, 243);
const FLinearColor TextColor = Color(235, 241, 247);
const FLinearColor MutedColor = Color(169, 188, 205);

bool SaveAsset(UObject* Asset)
{
    Asset->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    const FString Filename = FPackageName::LongPackageNameToFilename(
        Asset->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
    return UPackage::SavePackage(Asset->GetPackage(), Asset, *Filename, Args);
}

template<typename T>
T* LoadOrCreate(const FString& PackageName, bool& bCreated, FString& Error)
{
    bCreated = false;
    const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
    if (auto* Existing = LoadObject<UObject>(nullptr, *(PackageName + TEXT(".") + Name)))
    {
        auto* Asset = Cast<T>(Existing);
        if (!Asset) Error = TEXT("Unexpected asset class: ") + PackageName;
        return Asset;
    }
    auto* Asset = NewObject<T>(CreatePackage(*PackageName), *Name, RF_Public | RF_Standalone | RF_Transactional);
    if (!Asset) { Error = TEXT("Could not create: ") + PackageName; return nullptr; }
    FAssetRegistryModule::AssetCreated(Asset);
    bCreated = true;
    return Asset;
}

bool EnsureInputAssets(FString& Error)
{
    bool bNewAction = false;
    auto* Action = LoadOrCreate<UInputAction>(Folder / TEXT("Input/IA_ToggleAiInspector"), bNewAction, Error);
    if (!Action) return false;
    if (bNewAction)
    {
        Action->ValueType = EInputActionValueType::Boolean;
        Action->bConsumeInput = true;
        if (!SaveAsset(Action)) { Error = TEXT("Could not save IA_ToggleAiInspector"); return false; }
    }
    else if (Action->ValueType != EInputActionValueType::Boolean)
    {
        Error = TEXT("IA_ToggleAiInspector must use Boolean; existing editor changes were kept");
        return false;
    }
    bool bNewMapping = false;
    auto* Mapping = LoadOrCreate<UInputMappingContext>(Folder / TEXT("Input/IMC_AiInspector"), bNewMapping, Error);
    if (!Mapping) return false;
    const bool bHasBinding = Mapping->GetMappings().ContainsByPredicate([Action](const FEnhancedActionKeyMapping& Entry)
        { return Entry.Action == Action && Entry.Key == EKeys::F6; });
    if (!bHasBinding)
    {
        Mapping->Modify();
        Mapping->MapKey(Action, EKeys::F6);
    }
    if ((bNewMapping || !bHasBinding) && !SaveAsset(Mapping))
    {
        Error = TEXT("Could not save IMC_AiInspector");
        return false;
    }
    return true;
}

UTextBlock* MakeText(UWidgetTree* Tree, UFont* Font, FName Name, const TCHAR* Text,
    int32 Size, FLinearColor Tint, bool bWrap = false)
{
    auto* Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
    Label->bIsVariable = !Name.IsNone();
    Label->SetText(FText::FromString(Text));
    Label->SetFont(FSlateFontInfo(Font, Size, TEXT("Regular")));
    Label->SetColorAndOpacity(FSlateColor(Tint));
    Label->SetAutoWrapText(bWrap);
    Label->SetVisibility(ESlateVisibility::HitTestInvisible);
    if (!bWrap) Label->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
    return Label;
}

UButton* MakeButton(UWidgetTree* Tree, UFont* Font, FName Name, FName LabelName,
    const TCHAR* Text, int32 FontSize = 16)
{
    auto* Button = Tree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
    Button->bIsVariable = true;
    FButtonStyle Style = Button->GetStyle();
    Style.SetNormal(FSlateRoundedBoxBrush(InsetColor, 5.f, BorderColor, .7f));
    Style.SetHovered(FSlateRoundedBoxBrush(Color(59, 88, 112), 5.f, AccentColor, 1.f));
    Style.SetPressed(FSlateRoundedBoxBrush(Color(39, 65, 87), 5.f, AccentColor, 1.f));
    Style.SetDisabled(FSlateRoundedBoxBrush(Color(62, 94, 121), 5.f, AccentColor, 1.f));
    Style.SetNormalPadding(FMargin(7, 5));
    Style.SetPressedPadding(FMargin(7, 6, 7, 4));
    Button->SetStyle(Style);
    Button->SetContent(MakeText(Tree, Font, LabelName, Text, FontSize, TextColor));
    return Button;
}

void AddRow(UVerticalBox* Stack, UWidget* Widget, float Bottom = 6.f)
{
    Stack->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0, 0, 0, Bottom));
}

void BuildLayout(UWidgetBlueprint* Blueprint, UFont* Font)
{
    Blueprint->Modify();
    if (Blueprint->WidgetTree)
        Blueprint->WidgetTree->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    auto* Tree = NewObject<UWidgetTree>(Blueprint, TEXT("WidgetTree"), RF_Transactional);
    Blueprint->WidgetTree = Tree;

    auto* Size = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("InspectorSize"));
    Size->bIsVariable = true;
    Size->SetWidthOverride(330.f);
    Size->SetMaxDesiredHeight(540.f);
    Size->SetClipping(EWidgetClipping::ClipToBounds);
    Size->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
    Tree->RootWidget = Size;
    auto* Frame = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("InspectorFrame"));
    Frame->SetBrush(FSlateRoundedBoxBrush(PanelColor, 10.f, BorderColor, 1.f));
    Frame->SetPadding(FMargin(12));
    Frame->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
    Size->SetContent(Frame);
    auto* Stack = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InspectorStack"));
    Frame->SetContent(Stack);

    auto* Title = MakeText(Tree, Font, TEXT("InspectorTitle"), TEXT("LLM NPC OBESERVATION"), 18, TextColor, true);
    AddRow(Stack, Title, 8);
    auto* Expand = MakeButton(Tree, Font, TEXT("ExpandButton"), TEXT("ExpandLabel"), TEXT("F6 展开"), 16);
    Expand->SetToolTipText(FText::FromString(TEXT("LLM NPC OBESERVATION")));

    auto* Identity = Tree->ConstructWidget<UHorizontalBox>();
    auto* IdentityLabels = Tree->ConstructWidget<UVerticalBox>();
    AddRow(IdentityLabels, MakeText(Tree, Font, TEXT("NpcName"), TEXT("尚未选择人物"), 18, TextColor), 2);
    AddRow(IdentityLabels, MakeText(Tree, Font, TEXT("StageLabel"), TEXT("暂无记录"), 16, AccentColor), 0);
    auto* NameSlot = Identity->AddChildToHorizontalBox(IdentityLabels);
    NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    NameSlot->SetPadding(FMargin(0, 0, 8, 0));
    Identity->AddChildToHorizontalBox(Expand)->SetVerticalAlignment(VAlign_Center);
    AddRow(Stack, Identity, 10);
    auto* Historical = MakeText(Tree, Font, TEXT("HistoryBanner"), TEXT("历史快照 · 不随当前状态更新"), 16, AccentColor, true);
    Historical->SetVisibility(ESlateVisibility::Collapsed);
    AddRow(Stack, Historical);
    auto* Latest = MakeButton(Tree, Font, TEXT("LatestButton"), NAME_None, TEXT("返回最新记录"));
    Latest->SetVisibility(ESlateVisibility::Collapsed);
    AddRow(Stack, Latest);

    auto* Compact = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CompactSummary"));
    Compact->bIsVariable = true;
    for (const auto& Entry : { TPair<FName, const TCHAR*>(TEXT("CompactProposed"), TEXT("模型建议  未提供")),
        TPair<FName, const TCHAR*>(TEXT("CompactExecution"), TEXT("执行结果  未提供")) })
    {
        auto* Line = Tree->ConstructWidget<USizeBox>();
        Line->SetMaxDesiredHeight(48.f);
        Line->SetClipping(EWidgetClipping::ClipToBounds);
        auto* Text = MakeText(Tree, Font, Entry.Key, Entry.Value, 18, MutedColor, true);
        Text->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
        Line->SetContent(Text);
        AddRow(Compact, Line);
    }
    Compact->SetVisibility(ESlateVisibility::Collapsed);
    AddRow(Stack, Compact, 0);
    AddRow(Stack, MakeText(Tree, Font, TEXT("EmptyLabel"), TEXT("尚无本轮记录"), 18, MutedColor), 2);

    auto* Expanded = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ExpandedPanel"));
    Expanded->bIsVariable = true;
    Expanded->SetVisibility(ESlateVisibility::Collapsed);
    Stack->AddChildToVerticalBox(Expanded)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    auto* Tabs = Tree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass(), TEXT("InspectorTabs"));
    Tabs->SetInnerSlotPadding(FVector2D(5, 5));
    const TCHAR* TabNames[] = { TEXT("OverviewTab"), TEXT("PerceptionTab"), TEXT("ActionsTab"), TEXT("TimingTab"), TEXT("HistoryTab") };
    const TCHAR* TabLabels[] = { TEXT("概览"), TEXT("感知输入"), TEXT("候选执行"), TEXT("语音耗时"), TEXT("最近轮次") };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(TabNames); ++Index)
        Tabs->AddChildToWrapBox(MakeButton(Tree, Font, TabNames[Index], NAME_None, TabLabels[Index], 16));
    AddRow(Expanded, Tabs, 8);
    auto* DetailsArea = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("DetailsArea"));
    DetailsArea->bIsVariable = true;
    DetailsArea->SetClipping(EWidgetClipping::ClipToBounds);
    Expanded->AddChildToVerticalBox(DetailsArea)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    auto* DetailsFrame = Tree->ConstructWidget<UBorder>();
    DetailsFrame->SetBrush(FSlateRoundedBoxBrush(InsetColor, 6.f));
    DetailsFrame->SetPadding(FMargin(10));
    DetailsArea->SetContent(DetailsFrame);
    auto* Scroll = Tree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("DetailsScroll"));
    Scroll->bIsVariable = true;
    Scroll->SetScrollBarVisibility(ESlateVisibility::Visible);
    Scroll->SetAllowOverscroll(false);
    DetailsFrame->SetContent(Scroll);
    auto* Pages = Tree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass(), TEXT("TabPages"));
    Pages->bIsVariable = true;
    Scroll->AddChild(Pages);
    const TCHAR* PageNames[] = { TEXT("OverviewText"), TEXT("PerceptionText"), TEXT("ActionText"), TEXT("TimingText") };
    for (const auto* PageName : PageNames)
        Pages->AddChild(MakeText(Tree, Font, PageName, TEXT("尚无本轮记录"), 18, TextColor, true));
    auto* History = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("HistoryPage"));
    Pages->AddChild(History);
    AddRow(History, MakeText(Tree, Font, TEXT("HistoryEmptyLabel"), TEXT("尚无已保存的轮次"), 18, MutedColor, true));
    for (int32 Index = 0; Index < 8; ++Index)
    {
        auto* Button = MakeButton(Tree, Font, *FString::Printf(TEXT("HistoryButton%d"), Index),
            *FString::Printf(TEXT("HistoryText%d"), Index), TEXT(""), 18);
        if (auto* Label = Cast<UTextBlock>(Button->GetContent())) Label->SetAutoWrapText(true);
        Button->SetVisibility(ESlateVisibility::Collapsed);
        AddRow(History, Button, 6);
    }
    Pages->SetActiveWidgetIndex(0);

    // 重建控件树时同步变量身份，避免残留旧 GUID 造成蓝图编译错误
    TSet<FName> Names;
    Blueprint->ForEachSourceWidget([Blueprint, &Names](UWidget* Widget)
    {
        Names.Add(Widget->GetFName());
        if (!Blueprint->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
            Blueprint->WidgetVariableNameToGuidMap.Add(Widget->GetFName(), FGuid::NewGuid());
    });
    for (const auto& Animation : Blueprint->Animations)
        if (Animation)
        {
            Names.Add(Animation->GetFName());
            if (!Blueprint->WidgetVariableNameToGuidMap.Contains(Animation->GetFName()))
                Blueprint->WidgetVariableNameToGuidMap.Add(Animation->GetFName(), FGuid::NewGuid());
        }
    for (auto It = Blueprint->WidgetVariableNameToGuidMap.CreateIterator(); It; ++It)
        if (!Names.Contains(It.Key())) It.RemoveCurrent();
}
}

FString UDeliveryAiInspectorAssetTools::CreateAiInspectorWidget(bool bReplaceLayout)
{
    if (GEditor && GEditor->PlayWorld) return TEXT("Stop PIE before creating AI inspector assets");
    auto* Existing = LoadObject<UObject>(nullptr, *(Folder / (BlueprintName + TEXT(".") + BlueprintName)));
    auto* Blueprint = Cast<UWidgetBlueprint>(Existing);
    if (Existing && (!Blueprint || Blueprint->ParentClass != UDeliveryAiInspectorWidget::StaticClass()))
        return TEXT("WBP_AiInspector has an unexpected asset type or parent class");
    FString Error;
    if (!EnsureInputAssets(Error)) return Error;
    if (Blueprint && !bReplaceLayout) return TEXT("AI inspector input ready; existing WBP layout kept");
    auto* Font = LoadObject<UFont>(nullptr, TEXT("/Game/UI/Conversation/F_Conversation.F_Conversation"));
    if (!Font) return TEXT("F_Conversation is missing; restore the shared font before creating WBP_AiInspector");
    if (!Blueprint)
    {
        auto* Factory = NewObject<UWidgetBlueprintFactory>();
        Factory->ParentClass = UDeliveryAiInspectorWidget::StaticClass();
        Blueprint = Cast<UWidgetBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(
            BlueprintName, Folder, UWidgetBlueprint::StaticClass(), Factory));
    }
    if (!Blueprint) return TEXT("WBP_AiInspector creation failed");
    BuildLayout(Blueprint, Font);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
    if (Blueprint->Status == BS_Error) return TEXT("WBP_AiInspector compilation failed; asset was not saved");
    if (!SaveAsset(Blueprint)) return TEXT("WBP_AiInspector save failed");
    return TEXT("Saved WBP_AiInspector, IA_ToggleAiInspector, IMC_AiInspector");
}
