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

const FLinearColor Navy=Color(25,39,88);
const FLinearColor Cream=Color(255,246,226);
const FLinearColor Blue=Color(46,101,215);
const FLinearColor Yellow=Color(255,208,49);
const FLinearColor Coral=Color(255,126,116);

FSlateBrush Rounded(FLinearColor Fill,float Radius=14.f,float Outline=2.f)
{
    return FSlateRoundedBoxBrush(Fill,Radius,Navy,Outline);
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
    Style.SetHovered(Rounded(FMath::Lerp(Fill,FLinearColor::White,.16f),14,3));
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

    auto* Scale=Tree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass(),TEXT("PanelScale"));
    Scale->SetStretch(EStretch::ScaleToFit);
    Scale->SetStretchDirection(EStretchDirection::Both);
    Tree->RootWidget=Scale;
    auto* DesignSize=Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass(),TEXT("PanelDesignSize"));
    DesignSize->SetWidthOverride(440);
    DesignSize->SetHeightOverride(330);
    Scale->SetContent(DesignSize);

    // 颜色、字体和尺寸写入蓝图资产，之后可直接在 UMG 编辑器修改
    auto* Frame=Tree->ConstructWidget<UBorder>(UBorder::StaticClass(),TEXT("PanelFrame"));
    Frame->SetBrush(Rounded(Cream,22,3));
    Frame->SetPadding(FMargin(12));
    DesignSize->SetContent(Frame);
    auto* Rows=Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(),TEXT("ConversationRows"));
    Frame->SetContent(Rows);

    auto* Header=Tree->ConstructWidget<UBorder>(UBorder::StaticClass(),TEXT("NameBar"));
    Header->SetBrush(Rounded(Blue,14,0));
    Header->SetPadding(FMargin(12,7));
    auto* HeaderRow=Tree->ConstructWidget<UHorizontalBox>();
    Header->SetContent(HeaderRow);
    auto* Speaker=MakeText(Tree,Font,TEXT("SpeakerName"),TEXT("大嗓门"),22,Cream,true);
    Speaker->bIsVariable=true;
    auto* NameSlot=HeaderRow->AddChildToHorizontalBox(Speaker);
    NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    NameSlot->SetVerticalAlignment(VAlign_Center);
    HeaderRow->AddChildToHorizontalBox(MakeButton(Tree,Font,TEXT("CloseButton"),TEXT("结束"),Cream,Navy,16));
    AddRow(Rows,Header);

    auto* ReplyFrame=Tree->ConstructWidget<UBorder>(UBorder::StaticClass(),TEXT("ReplyFrame"));
    ReplyFrame->SetBrush(Rounded(Color(255,251,242),16,1));
    ReplyFrame->SetPadding(FMargin(12,8));
    auto* Scroll=Tree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(),TEXT("ReplyScroll"));
    Scroll->bIsVariable=true;
    Scroll->SetScrollBarVisibility(ESlateVisibility::Visible);
    Scroll->SetAllowOverscroll(false);
    ReplyFrame->SetContent(Scroll);
    auto* Reply=MakeText(Tree,Font,TEXT("Reply"),TEXT("先把话说清楚，再走也不迟。"),20,Navy,true);
    Reply->bIsVariable=true;
    Reply->SetAutoWrapText(true);
    Scroll->AddChild(Reply);
    auto* ReplySlot=Rows->AddChildToVerticalBox(ReplyFrame);
    ReplySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    ReplySlot->SetPadding(FMargin(0,0,0,8));

    auto* InputRow=Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(),TEXT("InputRow"));
    auto* Input=Tree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(),TEXT("Input"));
    Input->bIsVariable=true;
    Input->SetHintText(FText::FromString(TEXT("想说什么？")));
    FEditableTextBoxStyle InputStyle=Input->GetWidgetStyle();
    InputStyle.SetBackgroundImageNormal(Rounded(Color(255,251,242),12,1));
    InputStyle.SetBackgroundImageHovered(Rounded(FLinearColor::White,12,2));
    InputStyle.SetBackgroundImageFocused(Rounded(FLinearColor::White,12,3));
    InputStyle.SetPadding(FMargin(10,8));
    InputStyle.SetTextStyle(FTextBlockStyle().SetFont(FontInfo(Font,18,Navy)).SetColorAndOpacity(Navy));
    InputStyle.SetForegroundColor(Navy);
    InputStyle.SetFocusedForegroundColor(Navy);
    Input->SetWidgetStyle(InputStyle);
    auto* InputSlot=InputRow->AddChildToHorizontalBox(Input);
    InputSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    InputSlot->SetPadding(FMargin(0,0,8,0));
    InputRow->AddChildToHorizontalBox(MakeButton(Tree,Font,TEXT("SendButton"),TEXT("发送"),Blue,Cream));
    AddRow(Rows,InputRow);

    auto* Shortcuts=Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(),TEXT("ShortcutRow"));
    auto* ApologyIcon=LoadObject<UTexture2D>(nullptr,TEXT("/Game/UI/Conversation/T_Apology.T_Apology"));
    auto* ProvokeIcon=LoadObject<UTexture2D>(nullptr,TEXT("/Game/UI/Conversation/T_Provoke.T_Provoke"));
    auto* Apology=Shortcuts->AddChildToHorizontalBox(MakeButton(Tree,Font,TEXT("ApologizeButton"),TEXT("道歉"),Yellow,Navy,22,ApologyIcon));
    Apology->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    Apology->SetPadding(FMargin(0,0,4,0));
    auto* Provoke=Shortcuts->AddChildToHorizontalBox(MakeButton(Tree,Font,TEXT("ProvokeButton"),TEXT("拱火"),Coral,Navy,22,ProvokeIcon));
    Provoke->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    Provoke->SetPadding(FMargin(4,0,0,0));
    AddRow(Rows,Shortcuts);
    auto* Hint=MakeText(Tree,Font,TEXT("CloseHint"),TEXT("Esc  结束交谈"),14,Color(88,94,119));
    Hint->SetJustification(ETextJustify::Center);
    AddRow(Rows,Hint,0);

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
