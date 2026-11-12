#include "UI/DeliveryTrunkStatusWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/ProgressBar.h"

TSharedRef<SWidget> UDeliveryTrunkStatusWidget::RebuildWidget()
{
    if (WidgetTree && !Label)
    {
        auto* Root=WidgetTree->ConstructWidget<UCanvasPanel>(); WidgetTree->RootWidget=Root;
        auto* Panel=WidgetTree->ConstructWidget<UBorder>();
        Panel->SetBrushColor(FLinearColor(.025f,.04f,.06f,.9f)); Panel->SetPadding(FMargin(16));
        auto* PanelSlot=Root->AddChildToCanvas(Panel);
        PanelSlot->SetAnchors(FAnchors(.5f,.8f)); PanelSlot->SetAlignment(FVector2D(.5,.5)); PanelSlot->SetSize(FVector2D(560,90));
        auto* Column=WidgetTree->ConstructWidget<UVerticalBox>(); Panel->SetContent(Column);
        Label=WidgetTree->ConstructWidget<UTextBlock>(); Label->SetJustification(ETextJustify::Center);
        Column->AddChildToVerticalBox(Label);
        Bar=WidgetTree->ConstructWidget<UProgressBar>(); Bar->SetFillColorAndOpacity(FLinearColor(.2f,.8f,.65f));
        Column->AddChildToVerticalBox(Bar);
    }
    return Super::RebuildWidget();
}
void UDeliveryTrunkStatusWidget::SetStatus(const FText& Text,float Progress)
{
    if (Label) Label->SetText(Text);
    if (Bar) Bar->SetPercent(Progress);
}
