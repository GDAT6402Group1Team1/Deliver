#include "DeliveryNpcCaptionAssetTools.h"

#include "UI/Conversation/DeliveryNpcCaptionWidget.h"
#include "Animation/WidgetAnimation.h"
#include "AssetToolsModule.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "Engine/Font.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

FString UDeliveryNpcCaptionAssetTools::CreateNpcCaptionWidget(bool bReplaceLayout)
{
    if (GEditor && GEditor->PlayWorld) return TEXT("Stop PIE before creating NPC caption assets");
    const FString Folder = TEXT("/Game/UI/Conversation");
    const FString Name = TEXT("WBP_NpcCaption");
    const FString Path = Folder / TEXT("WBP_NpcCaption.WBP_NpcCaption");
    auto* Existing = LoadObject<UObject>(nullptr, *Path);
    auto* Blueprint = Cast<UWidgetBlueprint>(Existing);
    if (Existing && (!Blueprint || Blueprint->ParentClass != UDeliveryNpcCaptionWidget::StaticClass()))
        return TEXT("NPC caption blueprint has an unexpected asset type or parent class");
    if (Blueprint && !bReplaceLayout) return TEXT("NPC caption layout already exists; kept editor changes");
    auto* Font = LoadObject<UFont>(nullptr, TEXT("/Game/UI/Conversation/F_Conversation.F_Conversation"));
    if (!Font) return TEXT("F_Conversation is missing; restore the shared font before creating NPC captions");
    if (!Blueprint)
    {
        auto* Factory = NewObject<UWidgetBlueprintFactory>();
        Factory->ParentClass = UDeliveryNpcCaptionWidget::StaticClass();
        Blueprint = Cast<UWidgetBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(
            Name, Folder, UWidgetBlueprint::StaticClass(), Factory));
    }
    if (!Blueprint) return TEXT("NPC caption blueprint creation failed");

    Blueprint->Modify();
    if (Blueprint->WidgetTree)
        Blueprint->WidgetTree->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    auto* Tree = NewObject<UWidgetTree>(Blueprint, TEXT("WidgetTree"), RF_Transactional);
    Blueprint->WidgetTree = Tree;

    const FLinearColor Fill = FLinearColor::FromSRGBColor(FColor(238, 244, 248));
    const FLinearColor Ink = FLinearColor::FromSRGBColor(FColor(23, 44, 67));
    const FLinearColor Outline = FLinearColor::FromSRGBColor(FColor(41, 70, 94));
    auto* Frame = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("CaptionFrame"));
    Frame->SetBrush(FSlateRoundedBoxBrush(Fill, 16.f, Outline, 2.f));
    Frame->SetPadding(FMargin(14.f, 8.f));
    Frame->SetVisibility(ESlateVisibility::HitTestInvisible);
    Tree->RootWidget = Frame;

    auto* Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Caption"));
    Label->bIsVariable = true;
    Label->SetText(FText::FromString(TEXT("居民：先别急，听我说。")));
    // 中文保留原字形，不叠加字体描边
    Label->SetFont(FSlateFontInfo(Font, 18, TEXT("Regular")));
    Label->SetColorAndOpacity(FSlateColor(Ink));
    Label->SetWrapTextAt(380.f);
    Label->SetJustification(ETextJustify::Center);
    Label->SetVisibility(ESlateVisibility::HitTestInvisible);
    Frame->SetContent(Label);

    // 重建时同步控件身份，保留现有动画变量并清除旧树的名称
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

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
    if (Blueprint->Status == BS_Error) return TEXT("NPC caption blueprint compilation failed; asset was not saved");
    Blueprint->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    const FString Filename = FPackageName::LongPackageNameToFilename(
        Blueprint->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Blueprint->GetPackage(), Blueprint, *Filename, Args))
        return TEXT("NPC caption blueprint save failed");
    return TEXT("NPC caption blueprint saved: /Game/UI/Conversation/WBP_NpcCaption");
}
