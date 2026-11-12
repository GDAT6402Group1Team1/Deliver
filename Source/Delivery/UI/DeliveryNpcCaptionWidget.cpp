#include "UI/DeliveryNpcCaptionWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/Border.h"

void UDeliveryNpcCaptionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (Caption) return;
    auto* Border=WidgetTree->ConstructWidget<UBorder>(); Border->SetBrushColor(FLinearColor(0,0,0,.75f)); Border->SetPadding(FMargin(10,5));
    Caption=WidgetTree->ConstructWidget<UTextBlock>(); Caption->SetColorAndOpacity(FSlateColor(FLinearColor::White));
    Caption->SetWrapTextAt(380); Caption->SetJustification(ETextJustify::Center);
    auto Font=Caption->GetFont(); Font.Size=18; Caption->SetFont(Font);
    Border->SetContent(Caption); WidgetTree->RootWidget=Border;
}
void UDeliveryNpcCaptionWidget::SetCaption(const FText& Text)
{
    if (!Caption) NativeConstruct();
    Caption->SetText(Text); SetVisibility(Text.IsEmpty()?ESlateVisibility::Collapsed:ESlateVisibility::HitTestInvisible);
}
