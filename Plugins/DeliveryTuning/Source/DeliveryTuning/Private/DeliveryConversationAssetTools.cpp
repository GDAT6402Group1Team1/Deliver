#include "DeliveryConversationAssetTools.h"
#include "UI/DeliveryConversationWidget.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/Image.h"
#include "Engine/Texture2D.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

namespace
{
FLinearColor Color(uint8 Red,uint8 Green,uint8 Blue)
{
    return FLinearColor::FromSRGBColor(FColor(Red,Green,Blue));
}

const FLinearColor Ink=Color(45,48,42);
const FLinearColor Paper=Color(244,241,232);
const FLinearColor Sage=Color(128,140,109);
const FLinearColor Muted=Color(193,198,184);

FSlateBrush Rounded(FLinearColor Fill,float Radius=8.f,float Outline=0.f)
{
    return FSlateRoundedBoxBrush(Fill,Radius,Color(137,143,127),Outline);
}

UFont* GetConversationFont()
{
    const FString PackageName=TEXT("/Game/UI/Conversation/F_Conversation");
    if (auto* Existing=LoadObject<UFont>(nullptr,*(PackageName+TEXT(".F_Conversation")))) return Existing;
    auto* Face=LoadObject<UFontFace>(nullptr,TEXT("/Engine/EngineFonts/Faces/DroidSansFallback.DroidSansFallback"));
    if (!Face) return nullptr;
    auto* Package=CreatePackage(*PackageName);
    auto* Font=NewObject<UFont>(Package,TEXT("F_Conversation"),RF_Public|RF_Standalone|RF_Transactional);
    Font->FontCacheType=EFontCacheType::Runtime;
    auto& Typeface=Font->GetMutableInternalCompositeFont().DefaultTypeface;
    FTypefaceEntry Regular(TEXT("Regular"));
    Regular.Font=FFontData(Face);
    Typeface.Fonts.Add(Regular);
    FAssetRegistryModule::AssetCreated(Font);
    Font->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags=RF_Public|RF_Standalone;
    const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Package,Font,*Filename,Args)) return nullptr;
    return Font;
}

FSlateFontInfo FontInfo(UFont* Font,int32 Size,FLinearColor Tint,bool bBold=false)
{
    FFontOutlineSettings Outline;
    // 中文笔画较密，使用字体原有线条保持小字号清晰
    Outline.OutlineSize=0;
    Outline.OutlineColor=Tint;
    return FSlateFontInfo(Font,Size,TEXT("Regular"),Outline);
}

UTextBlock* MakeText(UWidgetTree* Tree,UFont* Font,FName Name,const TCHAR* Text,int32 Size,FLinearColor Tint,bool bBold=false)
{
    auto* Label=Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),Name);
    Label->SetText(FText::FromString(Text));
    Label->SetFont(FontInfo(Font,Size,Tint,bBold));
    Label->SetColorAndOpacity(FSlateColor(Tint));
    return Label;
}

UButton* MakeButton(UWidgetTree* Tree,UFont* Font,FName Name,const TCHAR* Text,FLinearColor Fill,FLinearColor TextColor,int32 FontSize=20,UTexture2D* Icon=nullptr)
{
    auto* Button=Tree->ConstructWidget<UButton>(UButton::StaticClass(),Name);
    Button->bIsVariable=true;
    FButtonStyle Style=Button->GetStyle();
    Style.SetNormal(Rounded(Fill));
    Style.SetHovered(Rounded(FMath::Lerp(Fill,FLinearColor::White,.10f),8,1));
    Style.SetPressed(Rounded(Fill*.78f));
    Style.SetDisabled(Rounded(Fill*.65f));
    Style.SetNormalPadding(FMargin(12,7));
    Style.SetPressedPadding(FMargin(12,10,12,4));
    Button->SetStyle(Style);
    auto* Label=MakeText(Tree,Font,NAME_None,Text,FontSize,TextColor,true);
    if (Icon)
    {
        auto* Row=Tree->ConstructWidget<UHorizontalBox>();
        auto* Size=Tree->ConstructWidget<USizeBox>();
        Size->SetWidthOverride(30); Size->SetHeightOverride(30);
        auto* Image=Tree->ConstructWidget<UImage>();
        Image->SetBrushFromTexture(Icon); Size->SetContent(Image);
        auto* IconSlot=Row->AddChildToHorizontalBox(Size);
        IconSlot->SetPadding(FMargin(0,0,7,0)); IconSlot->SetVerticalAlignment(VAlign_Center);
        Row->AddChildToHorizontalBox(Label)->SetVerticalAlignment(VAlign_Center);
        Button->SetContent(Row);
        if (auto* Slot=Cast<UButtonSlot>(Row->Slot)) Slot->SetHorizontalAlignment(HAlign_Center);
    }
    else Button->SetContent(Label);
    return Button;
}

void AddRow(UVerticalBox* Box,UWidget* Child,float BottomGap=8.f)
{
    auto* Slot=Box->AddChildToVerticalBox(Child);
    Slot->SetPadding(FMargin(0,0,0,BottomGap));
}
}

FString UDeliveryConversationAssetTools::CreateConversationWidget(bool bReplaceLayout)
{
    const FString Folder=TEXT("/Game/UI/Conversation");
    const FString Name=TEXT("WBP_Conversation");
    const FString Path=Folder/TEXT("WBP_Conversation.WBP_Conversation");
    auto* Blueprint=LoadObject<UWidgetBlueprint>(nullptr,*Path);
    if (Blueprint && !bReplaceLayout) return TEXT("Conversation layout already exists; kept editor changes");
    if (Blueprint && Blueprint->ParentClass!=UDeliveryConversationWidget::StaticClass())
        return TEXT("Conversation blueprint uses another parent class");
    auto* Font=GetConversationFont();
    if (!Font) return TEXT("Conversation font creation failed");
    if (!Blueprint)
    {
        auto* Factory=NewObject<UWidgetBlueprintFactory>();
        Factory->ParentClass=UDeliveryConversationWidget::StaticClass();
        Blueprint=Cast<UWidgetBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(
            Name,Folder,UWidgetBlueprint::StaticClass(),Factory));
    }
    if (!Blueprint) return TEXT("Conversation blueprint creation failed");
    Blueprint->Modify();
    if (Blueprint->WidgetTree)
        Blueprint->WidgetTree->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
    auto* Tree=NewObject<UWidgetTree>(Blueprint,TEXT("WidgetTree"),RF_Transactional);
    Blueprint->WidgetTree=Tree;

    // 布局保存到 UMG，运行时只改变文字和例句区的显示状态
    auto* Frame=Tree->ConstructWidget<UBorder>(UBorder::StaticClass(),TEXT("PanelFrame"));
    Frame->SetBrush(Rounded(FLinearColor(Ink.R,Ink.G,Ink.B,.88f),10));
    Frame->SetPadding(FMargin(16,12));
    Tree->RootWidget=Frame;
    auto* Rows=Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(),TEXT("ConversationRows"));
    Frame->SetContent(Rows);

    auto* Header=Tree->ConstructWidget<UHorizontalBox>();
    auto* Speaker=MakeText(Tree,Font,TEXT("SpeakerName"),TEXT("翠花"),18,Muted);
    Speaker->bIsVariable=true;
    auto* NameSlot=Header->AddChildToHorizontalBox(Speaker);
    NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    NameSlot->SetVerticalAlignment(VAlign_Center);
    Header->AddChildToHorizontalBox(MakeButton(Tree,Font,TEXT("CloseButton"),TEXT("收起"),FLinearColor::Transparent,Muted,14));
    AddRow(Rows,Header,4);

    auto* Scroll=Tree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(),TEXT("ReplyScroll"));
    Scroll->bIsVariable=true;
    Scroll->SetScrollBarVisibility(ESlateVisibility::Visible);
    Scroll->SetAllowOverscroll(false);
    auto* Reply=MakeText(Tree,Font,TEXT("Reply"),TEXT(""),20,Paper);
    Reply->bIsVariable=true;
    Reply->SetAutoWrapText(true);
    Scroll->AddChild(Reply);
    auto* ReplySlot=Rows->AddChildToVerticalBox(Scroll);
    ReplySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    ReplySlot->SetPadding(FMargin(0,0,0,10));

    auto* Suggestions=Tree->ConstructWidget<UMultiLineEditableTextBox>(UMultiLineEditableTextBox::StaticClass(),TEXT("Suggestions"));
    Suggestions->bIsVariable=true;
    Suggestions->SetIsReadOnly(true);
    Suggestions->SetAutoWrapText(true);
    Suggestions->SetVisibility(ESlateVisibility::Collapsed);
    FEditableTextBoxStyle SuggestionStyle=Suggestions->WidgetStyle;
    SuggestionStyle.SetBackgroundImageNormal(Rounded(Color(66,70,61)));
    SuggestionStyle.SetBackgroundImageHovered(Rounded(Color(66,70,61)));
    SuggestionStyle.SetBackgroundImageFocused(Rounded(Color(66,70,61),8,1));
    SuggestionStyle.SetBackgroundImageReadOnly(Rounded(Color(66,70,61)));
    SuggestionStyle.SetTextStyle(FTextBlockStyle().SetFont(FontInfo(Font,16,Paper)).SetColorAndOpacity(Paper));
    SuggestionStyle.SetForegroundColor(Paper);
    SuggestionStyle.SetReadOnlyForegroundColor(Paper);
    SuggestionStyle.SetFocusedForegroundColor(Paper);
    SuggestionStyle.SetPadding(FMargin(10,8));
    Suggestions->WidgetStyle=SuggestionStyle;
    AddRow(Rows,Suggestions,8);

    auto* InputRow=Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(),TEXT("InputRow"));
    auto* Input=Tree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(),TEXT("Input"));
    Input->bIsVariable=true;
    Input->SetHintText(FText::FromString(TEXT("说点什么")));
    FEditableTextBoxStyle InputStyle=Input->GetWidgetStyle();
    InputStyle.SetBackgroundImageNormal(Rounded(Paper));
    InputStyle.SetBackgroundImageHovered(Rounded(FLinearColor::White));
    InputStyle.SetBackgroundImageFocused(Rounded(FLinearColor::White,8,1));
    InputStyle.SetPadding(FMargin(10,8));
    InputStyle.SetTextStyle(FTextBlockStyle().SetFont(FontInfo(Font,18,Ink)).SetColorAndOpacity(Ink));
    InputStyle.SetForegroundColor(Ink);
    InputStyle.SetFocusedForegroundColor(Ink);
    Input->SetWidgetStyle(InputStyle);
    auto* InputSlot=InputRow->AddChildToHorizontalBox(Input);
    InputSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    InputSlot->SetPadding(FMargin(0,0,8,0));
    InputRow->AddChildToHorizontalBox(MakeButton(Tree,Font,TEXT("SendButton"),TEXT("发送"),Sage,Paper,18));
    AddRow(Rows,InputRow,4);

    auto* Footer=Tree->ConstructWidget<UHorizontalBox>();
    auto* Topics=MakeButton(Tree,Font,TEXT("SuggestionToggle"),TEXT("找话题 · 选句复制"),FLinearColor::Transparent,Muted,14);
    Topics->SetToolTipText(FText::FromString(TEXT("可以挑战、挑拨，也可以劝和。选中例句复制到输入框，改成自己的话再发送")));
    Footer->AddChildToHorizontalBox(Topics);
    auto* Hint=MakeText(Tree,Font,TEXT("CloseHint"),TEXT("Enter 发送 · Esc 收起"),13,Muted);
    Hint->SetJustification(ETextJustify::Right);
    auto* HintSlot=Footer->AddChildToHorizontalBox(Hint);
    HintSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    HintSlot->SetVerticalAlignment(VAlign_Center);
    AddRow(Rows,Footer,0);

    // 新增控件先登记变量编号，保留已有按钮在蓝图中的引用
    Blueprint->ForEachSourceWidget([Blueprint](UWidget* Widget)
    {
        if (!Blueprint->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
            Blueprint->WidgetVariableNameToGuidMap.Add(Widget->GetFName(),FGuid::NewGuid());
    });
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave);
    if (Blueprint->Status==BS_Error) return TEXT("Conversation blueprint compilation failed");
    Blueprint->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags=RF_Public|RF_Standalone;
    const FString Filename=FPackageName::LongPackageNameToFilename(Folder/Name,FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Blueprint->GetPackage(),Blueprint,*Filename,Args))
        return TEXT("Conversation blueprint save failed");
    return TEXT("Conversation blueprint saved: /Game/UI/Conversation/WBP_Conversation");
}
