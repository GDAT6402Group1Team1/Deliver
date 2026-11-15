#include "World/DeliveryNavigationAreas.h"

UDeliveryCompanionVehicleNavFilter::UDeliveryCompanionVehicleNavFilter()
{
    AddExcludedArea(UDeliveryPedestrianLinkArea::StaticClass());
}
