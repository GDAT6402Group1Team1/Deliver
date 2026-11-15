#include "DeliveryWidgetAssetLibrary.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "WidgetBlueprint.h"

bool UDeliveryWidgetAssetLibrary::RegisterWidget(UWidgetBlueprint* Blueprint, UWidget* Widget)
{
    if (!IsValid(Blueprint) || !IsValid(Widget) || Widget->GetOuter() != Blueprint->WidgetTree)
    {
        return false;
    }

    if (!Blueprint->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
    {
        Blueprint->Modify();
        Blueprint->OnVariableAdded(Widget->GetFName());
    }
    return true;
}
