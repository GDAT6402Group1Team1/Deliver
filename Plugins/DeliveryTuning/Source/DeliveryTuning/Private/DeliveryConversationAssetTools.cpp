#include "DeliveryConversationAssetTools.h"
#include "UI/Conversation/DeliveryConversationWidget.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/Image.h"
#include "Engine/Texture2D.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UObject/SavePackage.h"

namespace
{
FLinearColor Color(uint8 Red,uint8 Green,uint8 Blue,uint8 Alpha=255)
{
    return FLinearColor::FromSRGBColor(FColor(Red,Green,Blue,Alpha));
}

const FLinearColor Ink=Color(23,44,67);
const FLinearColor White=Color(242,246,250);
const FLinearColor Muted=Color(190,210,225);
const FLinearColor Slate=Color(127,168,199);
const FLinearColor Accent=Color(153,221,242);
const FLinearColor Charcoal=Color(48,70,89,250);
const FLinearColor InputFill=Color(221,231,239);
const FLinearColor BubbleFill=Color(238,244,248);
const FLinearColor BubbleOutline=Color(41,70,94);

FSlateBrush Rounded(FLinearColor Fill,float Radius=8.f,float Outline=0.f,FLinearColor OutlineColor=Slate)
{
    return FSlateRoundedBoxBrush(Fill,Radius,OutlineColor,Outline);
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
    const bool bClear=Fill.A<=0.f;
    const float OutlineWidth=bClear?0.f:2.f;
    const FLinearColor ButtonOutline=TextColor==Ink?BubbleOutline:Slate;
    Style.SetNormal(Rounded(Fill,12,OutlineWidth,ButtonOutline));
    Style.SetHovered(Rounded(FMath::Lerp(Fill,FLinearColor::White,.10f),12,OutlineWidth,ButtonOutline));
    Style.SetPressed(Rounded(bClear?Fill:FMath::Lerp(Fill,Ink,.12f),12,OutlineWidth,ButtonOutline));
    Style.SetDisabled(Rounded(bClear?Fill:FMath::Lerp(Fill,Charcoal,.25f),12,OutlineWidth,ButtonOutline));
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

FFrameNumber AnimationFrame(float Seconds)
{
    return FFrameNumber(FMath::RoundToInt(Seconds*24000.f));
}

UWidgetAnimation* MakeAnimation(UWidgetBlueprint* Blueprint,FName Name,float Duration)
{
    // 重建时只替换这三个反馈动画，其他手工动画仍保留在蓝图中
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
    Animation->MovieScene->SetTickResolutionDirectly(FFrameRate(24000,1));
    Animation->MovieScene->SetDisplayRate(FFrameRate(60,1));
    Animation->MovieScene->SetPlaybackRange(FFrameNumber(0),AnimationFrame(Duration).Value+1);
    Animation->MovieScene->GetEditorData().WorkStart=0;
    Animation->MovieScene->GetEditorData().WorkEnd=Duration;
    Animation->MovieScene->GetEditorData().ViewStart=0;
    Animation->MovieScene->GetEditorData().ViewEnd=Duration;
    Blueprint->Animations.Add(Animation);
    return Animation;
}

FGuid BindAnimationWidget(UWidgetAnimation* Animation,UWidget* Widget)
{
    const FGuid Guid=Animation->MovieScene->AddPossessable(Widget->GetName(),Widget->GetClass());
    FWidgetAnimationBinding Binding;
    Binding.AnimationGuid=Guid;
    Binding.WidgetName=Widget->GetFName();
    Animation->AnimationBindings.Add(Binding);
    return Guid;
}

UMovieScene2DTransformSection* MakeTransformSection(UWidgetAnimation* Animation,const FGuid& Binding,
    EMovieScene2DTransformChannel Channels)
{
    auto* Track=Animation->MovieScene->AddTrack<UMovieScene2DTransformTrack>(Binding);
    Track->SetPropertyNameAndPath(TEXT("RenderTransform"),TEXT("RenderTransform"));
    auto* Section=CastChecked<UMovieScene2DTransformSection>(Track->CreateNewSection());
    Section->SetRange(Animation->MovieScene->GetPlaybackRange());
    Section->SetCompletionMode(EMovieSceneCompletionMode::RestoreState);
    Section->SetMask(Channels);
    Track->AddSection(*Section);
    return Section;
}

FMovieSceneFloatChannel& MakeOpacityChannel(UWidgetAnimation* Animation,const FGuid& Binding)
{
    auto* Track=Animation->MovieScene->AddTrack<UMovieSceneFloatTrack>(Binding);
    Track->SetPropertyNameAndPath(TEXT("RenderOpacity"),TEXT("RenderOpacity"));
    auto* Section=CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection());
    Section->SetRange(Animation->MovieScene->GetPlaybackRange());
    Section->SetCompletionMode(EMovieSceneCompletionMode::RestoreState);
    Track->AddSection(*Section);
    return Section->GetChannel();
}

void MakeFeedbackAnimations(UWidgetBlueprint* Blueprint,UButton* Send,UBorder* Submitted,UWidget* Waiting)
{
    // 关键帧保存在 WBP，运行时只负责播放、循环和停止
    auto* SendAnimation=MakeAnimation(Blueprint,TEXT("SendFeedback"),.22f);
    const FGuid SendBinding=BindAnimationWidget(SendAnimation,Send);
    auto* SendTransform=MakeTransformSection(SendAnimation,SendBinding,EMovieScene2DTransformChannel::Scale);
    for (auto& Channel : SendTransform->Scale)
    {
        Channel.AddCubicKey(AnimationFrame(0),1.f);
        Channel.AddCubicKey(AnimationFrame(.07f),1.055f);
        Channel.AddCubicKey(AnimationFrame(.22f),1.f);
    }
    auto* FlashTrack=SendAnimation->MovieScene->AddTrack<UMovieSceneColorTrack>(SendBinding);
    FlashTrack->SetPropertyNameAndPath(TEXT("BackgroundColor"),TEXT("BackgroundColor"));
    auto* Flash=CastChecked<UMovieSceneColorSection>(FlashTrack->CreateNewSection());
    Flash->SetRange(SendAnimation->MovieScene->GetPlaybackRange());
    Flash->SetCompletionMode(EMovieSceneCompletionMode::RestoreState);
    FlashTrack->AddSection(*Flash);
    for (auto* Channel : {&Flash->GetRedChannel(),&Flash->GetGreenChannel(),&Flash->GetBlueChannel()})
    {
        Channel->AddCubicKey(AnimationFrame(0),1.f);
        Channel->AddCubicKey(AnimationFrame(.055f),1.22f);
        Channel->AddCubicKey(AnimationFrame(.22f),1.f);
    }
    Flash->GetAlphaChannel().SetDefault(1.f);

    auto* Arrive=MakeAnimation(Blueprint,TEXT("MessageArrive"),.18f);
    const FGuid MessageBinding=BindAnimationWidget(Arrive,Submitted);
    auto& MessageOpacity=MakeOpacityChannel(Arrive,MessageBinding);
    MessageOpacity.AddCubicKey(AnimationFrame(0),0.f);
    MessageOpacity.AddCubicKey(AnimationFrame(.18f),1.f);
    auto* MessageTransform=MakeTransformSection(Arrive,MessageBinding,EMovieScene2DTransformChannel::TranslationY);
    MessageTransform->Translation[1].AddCubicKey(AnimationFrame(0),5.f);
    MessageTransform->Translation[1].AddCubicKey(AnimationFrame(.18f),0.f);

    auto* Pulse=MakeAnimation(Blueprint,TEXT("WaitingPulse"),1.f);
    auto& PulseOpacity=MakeOpacityChannel(Pulse,BindAnimationWidget(Pulse,Waiting));
    PulseOpacity.AddCubicKey(AnimationFrame(0),.3f);
    PulseOpacity.AddCubicKey(AnimationFrame(.5f),1.f);
    PulseOpacity.AddCubicKey(AnimationFrame(1.f),.3f);
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

    // 高度由当前内容决定，没有发言时不留一块空白
    auto* Width=Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass(),TEXT("ConversationWidth"));
    Width->bIsVariable=true;
    Width->SetWidthOverride(640);
    Tree->RootWidget=Width;
    auto* Stack=Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(),TEXT("ConversationStack"));
    Width->SetContent(Stack);

    auto* Submitted=Tree->ConstructWidget<UBorder>(UBorder::StaticClass(),TEXT("SubmittedLine"));
    Submitted->bIsVariable=true;
    Submitted->SetBrush(Rounded(BubbleFill,16,2,BubbleOutline));
    Submitted->SetPadding(FMargin(14,8));
    Submitted->SetVisibility(ESlateVisibility::Collapsed);
    auto* Reply=MakeText(Tree,Font,TEXT("Reply"),TEXT(""),18,Ink);
    Reply->bIsVariable=true;
    Reply->SetAutoWrapText(true);
    Submitted->SetContent(Reply);
    auto* SubmittedSlot=Stack->AddChildToVerticalBox(Submitted);
    SubmittedSlot->SetHorizontalAlignment(HAlign_Center);
    SubmittedSlot->SetPadding(FMargin(0,0,0,8));

    // 输入区保存到 UMG，运行时只更新文字、宽度和焦点
    auto* Frame=Tree->ConstructWidget<UBorder>(UBorder::StaticClass(),TEXT("PanelFrame"));
    Frame->SetBrush(Rounded(Charcoal,16,2));
    Frame->SetPadding(FMargin(14,9));
    AddRow(Stack,Frame,4);
    auto* Rows=Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(),TEXT("ConversationRows"));
    Frame->SetContent(Rows);

    auto* Header=Tree->ConstructWidget<UHorizontalBox>();
    auto* Speaker=MakeText(Tree,Font,TEXT("SpeakerName"),TEXT("对 二狗 说"),18,White);
    Speaker->bIsVariable=true;
    auto* NameSlot=Header->AddChildToHorizontalBox(Speaker);
    NameSlot->SetVerticalAlignment(VAlign_Center);
    auto* Waiting=Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(),TEXT("WaitingIndicator"));
    Waiting->bIsVariable=true;
    Waiting->SetVisibility(ESlateVisibility::Collapsed);
    for (int32 Index=0;Index<3;++Index)
    {
        auto* DotSize=Tree->ConstructWidget<USizeBox>();
        DotSize->SetWidthOverride(6);
        DotSize->SetHeightOverride(6);
        auto* Dot=Tree->ConstructWidget<UBorder>();
        Dot->SetPadding(FMargin(0));
        Dot->SetBrush(Rounded(Accent,3));
        DotSize->SetContent(Dot);
        auto* DotSlot=Waiting->AddChildToHorizontalBox(DotSize);
        DotSlot->SetPadding(FMargin(3,0));
        DotSlot->SetVerticalAlignment(VAlign_Center);
    }
    auto* WaitingSlot=Header->AddChildToHorizontalBox(Waiting);
    WaitingSlot->SetPadding(FMargin(12,0,0,0));
    WaitingSlot->SetVerticalAlignment(VAlign_Center);
    Header->AddChildToHorizontalBox(Tree->ConstructWidget<USpacer>())->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    auto* Close=MakeButton(Tree,Font,TEXT("CloseButton"),TEXT("×"),FLinearColor::Transparent,Muted,18);
    Close->SetToolTipText(FText::FromString(TEXT("结束交谈 · Esc")));
    Header->AddChildToHorizontalBox(Close);
    AddRow(Rows,Header,4);

    auto* Suggestions=Tree->ConstructWidget<UMultiLineEditableTextBox>(UMultiLineEditableTextBox::StaticClass(),TEXT("Suggestions"));
    Suggestions->bIsVariable=true;
    Suggestions->SetIsReadOnly(true);
    Suggestions->SetAutoWrapText(true);
    Suggestions->SetVisibility(ESlateVisibility::Collapsed);
    FEditableTextBoxStyle SuggestionStyle=Suggestions->WidgetStyle;
    SuggestionStyle.SetBackgroundImageNormal(Rounded(BubbleFill,12,2,BubbleOutline));
    SuggestionStyle.SetBackgroundImageHovered(Rounded(BubbleFill,12,2,BubbleOutline));
    SuggestionStyle.SetBackgroundImageFocused(Rounded(BubbleFill,12,2,BubbleOutline));
    SuggestionStyle.SetBackgroundImageReadOnly(Rounded(BubbleFill,12,2,BubbleOutline));
    SuggestionStyle.SetTextStyle(FTextBlockStyle().SetFont(FontInfo(Font,16,Ink)).SetColorAndOpacity(Ink));
    SuggestionStyle.SetForegroundColor(Ink);
    SuggestionStyle.SetReadOnlyForegroundColor(Ink);
    SuggestionStyle.SetFocusedForegroundColor(Ink);
    SuggestionStyle.SetPadding(FMargin(10,8));
    Suggestions->WidgetStyle=SuggestionStyle;

    auto* InputRow=Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(),TEXT("InputRow"));
    auto* Input=Tree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(),TEXT("Input"));
    Input->bIsVariable=true;
    Input->SetHintText(FText::FromString(TEXT("输入你想说的话")));
    // Enter 发送后继续留在输入区，Esc 和下一句输入仍交给这个界面
    Input->SetClearKeyboardFocusOnCommit(false);
    FEditableTextBoxStyle InputStyle=Input->GetWidgetStyle();
    InputStyle.SetBackgroundImageNormal(Rounded(InputFill,12,2,BubbleOutline));
    InputStyle.SetBackgroundImageHovered(Rounded(InputFill,12,2,Slate));
    InputStyle.SetBackgroundImageFocused(Rounded(InputFill,12,3,Accent));
    InputStyle.SetPadding(FMargin(10,8));
    InputStyle.SetTextStyle(FTextBlockStyle().SetFont(FontInfo(Font,18,Ink)).SetColorAndOpacity(Ink));
    InputStyle.SetForegroundColor(Ink);
    InputStyle.SetFocusedForegroundColor(Ink);
    Input->SetWidgetStyle(InputStyle);
    auto* InputSlot=InputRow->AddChildToHorizontalBox(Input);
    InputSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    InputSlot->SetPadding(FMargin(0,0,8,0));
    auto* Send=MakeButton(Tree,Font,TEXT("SendButton"),TEXT("发送"),Accent,Ink,18);
    Send->SetRenderTransformPivot(FVector2D(.5f,.5f));
    Send->SetToolTipText(FText::FromString(TEXT("发送 · Enter")));
    InputRow->AddChildToHorizontalBox(Send);
    auto* Retry=MakeButton(Tree,Font,TEXT("RetryButton"),TEXT("重试"),Accent,Ink,18);
    Retry->SetVisibility(ESlateVisibility::Collapsed);
    auto* RetrySlot=InputRow->AddChildToHorizontalBox(Retry);
    RetrySlot->SetPadding(FMargin(8,0,0,0));
    AddRow(Rows,InputRow,0);

    auto* Footer=Tree->ConstructWidget<UHorizontalBox>();
    auto* VoiceToggle=MakeButton(Tree,Font,TEXT("VoiceToggleButton"),TEXT("与二狗连麦"),Charcoal,White,14);
    auto* VoiceLabel=CastChecked<UTextBlock>(VoiceToggle->GetContent());
    VoiceLabel->Rename(TEXT("VoiceToggleLabel"),Tree,REN_DontCreateRedirectors|REN_NonTransactional);
    VoiceLabel->bIsVariable=true;
    VoiceToggle->SetToolTipText(FText::FromString(TEXT("与自己的二狗连麦，自然说话即可。V 开关麦克风，不保存或上传录音")));
    Footer->AddChildToHorizontalBox(VoiceToggle);
    auto* VoiceHold=MakeButton(Tree,Font,TEXT("VoiceHoldButton"),TEXT("连麦中"),Ink,White,14);
    VoiceHold->SetVisibility(ESlateVisibility::Collapsed);
    VoiceHold->SetToolTipText(FText::FromString(TEXT("连麦会自动听取你的话")));
    auto* VoiceSlot=Footer->AddChildToHorizontalBox(VoiceHold);
    VoiceSlot->SetPadding(FMargin(6,0,6,0));
    auto* Topics=MakeButton(Tree,Font,TEXT("SuggestionToggle"),TEXT("看看例句"),Charcoal,White,14);
    Topics->SetToolTipText(FText::FromString(TEXT("可以挑战、挑拨，也可以劝和。选中例句复制到输入框，改成自己的话再发送")));
    Footer->AddChildToHorizontalBox(Topics);
    auto* FooterSlot=Stack->AddChildToVerticalBox(Footer);
    FooterSlot->SetHorizontalAlignment(HAlign_Center);
    FooterSlot->SetPadding(FMargin(0,0,0,4));
    AddRow(Stack,Suggestions,0);

    MakeFeedbackAnimations(Blueprint,Send,Submitted,Waiting);

    // 新增控件先登记变量编号，保留已有按钮在蓝图中的引用
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
    // 重建控件树后移除旧变量登记，保留仍然存在的控件和动画编号
    for (auto It=Blueprint->WidgetVariableNameToGuidMap.CreateIterator();It;++It)
        if (!VariableNames.Contains(It.Key())) It.RemoveCurrent();
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
