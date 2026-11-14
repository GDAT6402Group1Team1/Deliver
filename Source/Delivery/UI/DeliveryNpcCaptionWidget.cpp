#include "UI/DeliveryNpcCaptionWidget.h"
#include "Components/TextBlock.h"

void UDeliveryNpcCaptionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ApplyCaption();
}

void UDeliveryNpcCaptionWidget::SetCaption(const FText& Text)
{
    if (PendingCaption.EqualTo(Text)) return;
    PendingCaption = Text;
    ApplyCaption();
}

void UDeliveryNpcCaptionWidget::ApplyCaption()
{
    if (!Caption) return;
    Caption->SetText(PendingCaption);
    SetVisibility(PendingCaption.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}
