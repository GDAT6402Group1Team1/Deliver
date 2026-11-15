#include "DeliverySpeechAssetTools.h"
#include "UObject/UnrealType.h"

#include "DeliveryConversationAssetTools.h"
#include "UI/DeliverySpeechStatusWidget.h"
#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "Editor.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace
{
const FString SpeechFolder=TEXT("/Game/UI/Conversation");
const FString SpeechBlueprintName=TEXT("WBP_SpeechStatus");
const FString SpeechActionPath=TEXT("/Game/UI/Conversation/Input/IA_PushToTalk");
const FString SpeechMappingPath=TEXT("/Game/UI/Conversation/Input/IMC_VoiceConversation");

FLinearColor Color(uint8 Red,uint8 Green,uint8 Blue,uint8 Alpha=255)
{
    return FLinearColor::FromSRGBColor(FColor(Red,Green,Blue,Alpha));
}

// 沿用正式交谈界面的字体和颜色，控件尺寸与动画保存到蓝图后仍可调整
const FLinearColor Ink=Color(28,39,52);
const FLinearColor White=Color(235,242,249);
const FLinearColor Muted=Color(167,185,201);
const FLinearColor Slate=Color(88,113,136);
const FLinearColor Accent=Color(151,212,237);

bool SaveAsset(UObject* Asset)
{
    Asset->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags=RF_Public|RF_Standalone;
    const FString Filename=FPackageName::LongPackageNameToFilename(
        Asset->GetPackage()->GetName(),FPackageName::GetAssetPackageExtension());
    return UPackage::SavePackage(Asset->GetPackage(),Asset,*Filename,Args);
}

template<typename T>
T* FindOrCreateAsset(const FString& PackageName,bool& bCreated,FString& Error)
{
    bCreated=false;
    const FString Name=FPackageName::GetLongPackageAssetName(PackageName);
    if (auto* Existing=LoadObject<UObject>(nullptr,*(PackageName+TEXT(".")+Name)))
    {
        auto* Asset=Cast<T>(Existing);
        if (!Asset) Error=TEXT("Existing asset has an unexpected class: ")+PackageName;
        return Asset;
    }
    auto* Asset=NewObject<T>(CreatePackage(*PackageName),*Name,RF_Public|RF_Standalone|RF_Transactional);
    if (!Asset) { Error=TEXT("Failed to create asset: ")+PackageName; return nullptr; }
    FAssetRegistryModule::AssetCreated(Asset);
    bCreated=true;
    return Asset;
}

bool EnsureSpeechInput(FString& Error)
{
    bool bNewAction=false;
    auto* Action=FindOrCreateAsset<UInputAction>(SpeechActionPath,bNewAction,Error);
    if (!Action) return false;
    if (bNewAction)
    {
        Action->ValueType=EInputActionValueType::Boolean;
        Action->bConsumeInput=true;
        Action->Triggers.Reset();
        Action->Modifiers.Reset();
        if (!SaveAsset(Action)) { Error=TEXT("Failed to save IA_PushToTalk"); return false; }
    }
    else if (Action->ValueType!=EInputActionValueType::Boolean)
    {
        Error=TEXT("Existing IA_PushToTalk must use Boolean; kept editor changes");
        return false;
    }

    bool bNewMapping=false;
    auto* Mapping=FindOrCreateAsset<UInputMappingContext>(SpeechMappingPath,bNewMapping,Error);
    if (!Mapping) return false;
    const bool bHasVoiceKey=Mapping->GetMappings().ContainsByPredicate([Action](const FEnhancedActionKeyMapping& Entry)
    { return Entry.Action==Action && Entry.Key==EKeys::V; });
    if (!bHasVoiceKey)
    {
        // 专用 IMC 只补 V，不改全局输入，也不占用现有手柄按键
        Mapping->Modify();
        Mapping->MapKey(Action,EKeys::V);
    }
    if ((bNewMapping || !bHasVoiceKey) && !SaveAsset(Mapping))
    {
        Error=TEXT("Failed to save IMC_VoiceConversation");
        return false;
    }
    return true;
}

UTextBlock* MakeText(UWidgetTree* Tree,UFont* Font,FName Name,const TCHAR* Text,int32 Size,FLinearColor Tint)
{
    auto* Label=Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),Name);
    Label->SetText(FText::FromString(Text));
    Label->SetFont(FSlateFontInfo(Font,Size,TEXT("Regular")));
    Label->SetColorAndOpacity(FSlateColor(Tint));
    return Label;
}

void MakePulseAnimation(UWidgetBlueprint* Blueprint,UWidget* Dot)
{
    const FName Name=TEXT("VoiceStatusPulse");
    for (int32 Index=Blueprint->Animations.Num()-1;Index>=0;--Index)
    {
        auto* Existing=Blueprint->Animations[Index].Get();
        if (Existing && Existing->GetFName()==Name)
        {
            Blueprint->Animations.RemoveAt(Index);
            Existing->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
        }
    }
    auto* Animation=NewObject<UWidgetAnimation>(Blueprint,Name,RF_Transactional);
    Animation->SetDisplayLabel(Name.ToString());
    Animation->MovieScene=NewObject<UMovieScene>(Animation,Name,RF_Transactional);
    auto* Scene=Animation->MovieScene.Get();
    Scene->SetTickResolutionDirectly(FFrameRate(24000,1));
    Scene->SetDisplayRate(FFrameRate(60,1));
    Scene->SetPlaybackRange(FFrameNumber(0),24001);
    Scene->GetEditorData().WorkStart=0;
    Scene->GetEditorData().WorkEnd=1;
    Scene->GetEditorData().ViewStart=0;
    Scene->GetEditorData().ViewEnd=1;
    const FGuid Guid=Scene->AddPossessable(Dot->GetName(),Dot->GetClass());
    FWidgetAnimationBinding Binding;
    Binding.AnimationGuid=Guid;
    Binding.WidgetName=Dot->GetFName();
    Animation->AnimationBindings.Add(Binding);
    auto* Track=Scene->AddTrack<UMovieSceneFloatTrack>(Guid);
    Track->SetPropertyNameAndPath(TEXT("RenderOpacity"),TEXT("RenderOpacity"));
    auto* Section=CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection());
    Section->SetRange(Scene->GetPlaybackRange());
    Section->SetCompletionMode(EMovieSceneCompletionMode::RestoreState);
    Track->AddSection(*Section);
    auto& Opacity=Section->GetChannel();
    Opacity.AddCubicKey(FFrameNumber(0),.35f);
    Opacity.AddCubicKey(FFrameNumber(12000),1.f);
    Opacity.AddCubicKey(FFrameNumber(24000),.35f);
    Blueprint->Animations.Add(Animation);
}

void BuildStatusLayout(UWidgetBlueprint* Blueprint,UFont* Font,UTexture2D* MicOff,UTexture2D* MicOn)
{
    Blueprint->Modify();
    if (Blueprint->WidgetTree)
        Blueprint->WidgetTree->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
    auto* Tree=NewObject<UWidgetTree>(Blueprint,TEXT("WidgetTree"),RF_Transactional);
    Blueprint->WidgetTree=Tree;

    auto* Button=Tree->ConstructWidget<UButton>(UButton::StaticClass(),TEXT("VoiceDisableButton"));
    Tree->RootWidget=Button;
    Button->bIsVariable=true;
    // 鼠标可点，键盘仍由原 V 映射处理
    if (auto* FocusProperty=FindFProperty<FBoolProperty>(UButton::StaticClass(),TEXT("IsFocusable")))
        FocusProperty->SetPropertyValue_InContainer(Button,false);
    FButtonStyle Style=Button->GetStyle();
    Style.SetNormal(FSlateRoundedBoxBrush(Color(28,39,52,210),7.f));
    Style.SetHovered(FSlateRoundedBoxBrush(FMath::Lerp(Ink,Accent,.15f),7.f,Slate,1.f));
    Style.SetPressed(FSlateRoundedBoxBrush(Ink*.78f,7.f,Accent,1.f));
    Style.SetNormalPadding(FMargin(6,4));
    Style.SetPressedPadding(FMargin(6,4));
    Button->SetStyle(Style);

    auto* Row=Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(),TEXT("SpeechStatusRow"));
    Button->SetContent(Row);
    auto* IconSize=Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass(),TEXT("VoiceIconSize"));
    IconSize->SetWidthOverride(24.f);
    IconSize->SetHeightOverride(24.f);
    Row->AddChildToHorizontalBox(IconSize)->SetVerticalAlignment(VAlign_Center);
    auto* Icons=Tree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass(),TEXT("VoiceIcons"));
    Icons->bIsVariable=true;
    IconSize->SetContent(Icons);

    auto* OffIcon=Tree->ConstructWidget<UImage>(UImage::StaticClass(),TEXT("MicOffIcon"));
    OffIcon->SetBrushFromTexture(MicOff);
    OffIcon->SetColorAndOpacity(Muted);
    Icons->AddChild(OffIcon);
    auto* OnIcon=Tree->ConstructWidget<UImage>(UImage::StaticClass(),TEXT("MicOnIcon"));
    OnIcon->SetBrushFromTexture(MicOn);
    OnIcon->SetColorAndOpacity(Accent);
    Icons->AddChild(OnIcon);
    Icons->SetActiveWidgetIndex(0);

    auto* Key=MakeText(Tree,Font,TEXT("SpeechToggleKey"),TEXT("V"),14,White);
    auto* KeySlot=Row->AddChildToHorizontalBox(Key);
    KeySlot->SetPadding(FMargin(3,0,2,0));
    KeySlot->SetVerticalAlignment(VAlign_Center);
    MakePulseAnimation(Blueprint,Icons);

    TSet<FName> VariableNames;
    Blueprint->ForEachSourceWidget([Blueprint,&VariableNames](UWidget* Widget)
    {
        VariableNames.Add(Widget->GetFName());
        if (!Blueprint->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
            Blueprint->WidgetVariableNameToGuidMap.Add(Widget->GetFName(),FGuid::NewGuid());
    });
    for (const auto& Animation:Blueprint->Animations)
        if (Animation)
        {
            VariableNames.Add(Animation->GetFName());
            if (!Blueprint->WidgetVariableNameToGuidMap.Contains(Animation->GetFName()))
                Blueprint->WidgetVariableNameToGuidMap.Add(Animation->GetFName(),FGuid::NewGuid());
        }
    for (auto It=Blueprint->WidgetVariableNameToGuidMap.CreateIterator();It;++It)
        if (!VariableNames.Contains(It.Key())) It.RemoveCurrent();
}
}

FString UDeliverySpeechAssetTools::CreateSpeechAssets(bool bReplaceLayout)
{
    if (GEditor && GEditor->PlayWorld) return TEXT("Stop PIE before creating speech assets");
    const FString BlueprintPath=SpeechFolder/TEXT("WBP_SpeechStatus.WBP_SpeechStatus");
    auto* Existing=LoadObject<UObject>(nullptr,*BlueprintPath);
    auto* Blueprint=Cast<UWidgetBlueprint>(Existing);
    if (Existing && (!Blueprint || Blueprint->ParentClass!=UDeliverySpeechStatusWidget::StaticClass()))
        return TEXT("WBP_SpeechStatus uses an unexpected asset type or parent class");

    UDeliveryConversationAssetTools::CreateConversationWidget(false);
    auto* Font=LoadObject<UFont>(nullptr,TEXT("/Game/UI/Conversation/F_Conversation.F_Conversation"));
    if (!Font) return TEXT("Conversation font is missing; restore F_Conversation before creating speech UI");
    FString Error;
    if (!EnsureSpeechInput(Error)) return Error;
    if (Blueprint && !bReplaceLayout)
        return TEXT("Speech input assets ready; status layout already exists, kept editor changes");

    auto* MicOff=LoadObject<UTexture2D>(nullptr,TEXT("/Game/UI/Conversation/T_MicOff.T_MicOff"));
    auto* MicOn=LoadObject<UTexture2D>(nullptr,TEXT("/Game/UI/Conversation/T_MicOn.T_MicOn"));
    if (!MicOff || !MicOn) return TEXT("Import T_MicOff and T_MicOn before creating the speech status widget");

    if (!Blueprint)
    {
        auto* Factory=NewObject<UWidgetBlueprintFactory>();
        Factory->ParentClass=UDeliverySpeechStatusWidget::StaticClass();
        Blueprint=Cast<UWidgetBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(
            SpeechBlueprintName,SpeechFolder,UWidgetBlueprint::StaticClass(),Factory));
    }
    if (!Blueprint) return TEXT("Speech status blueprint creation failed");
    BuildStatusLayout(Blueprint,Font,MicOff,MicOn);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave);
    if (Blueprint->Status==BS_Error) return TEXT("Speech status blueprint compilation failed");
    if (!SaveAsset(Blueprint)) return TEXT("Speech status blueprint save failed");
    return TEXT("Speech assets saved: WBP_SpeechStatus, IA_PushToTalk, IMC_VoiceConversation");
}
