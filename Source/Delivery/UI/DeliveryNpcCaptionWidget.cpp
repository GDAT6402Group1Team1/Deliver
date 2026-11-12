#include "UI/DeliveryNpcCaptionWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/Border.h"

TSharedRef<SWidget> UDeliveryNpcCaptionWidget::RebuildWidget()
{
    // 根控件必须在 Slate 构建前建立，NativeConstruct 才赋值会留下空的头顶浮窗
    if (WidgetTree->RootWidget) return Super::RebuildWidget();
    auto* Border=WidgetTree->ConstructWidget<UBorder>(); Border->SetBrushColor(FLinearColor(0,0,0,.75f)); Border->SetPadding(FMargin(10,5));
    Caption=WidgetTree->ConstructWidget<UTextBlock>(); Caption->SetColorAndOpacity(FSlateColor(FLinearColor::White));
    Caption->SetWrapTextAt(380); Caption->SetJustification(ETextJustify::Center);
    auto Font=Caption->GetFont(); Font.Size=18; Caption->SetFont(Font);
    Border->SetContent(Caption); WidgetTree->RootWidget=Border;
    return Super::RebuildWidget();
}
void UDeliveryNpcCaptionWidget::SetCaption(const FText& Text)
{
    if (!Caption) return;
    Caption->SetText(Text); SetVisibility(Text.IsEmpty()?ESlateVisibility::Collapsed:ESlateVisibility::HitTestInvisible);
}
