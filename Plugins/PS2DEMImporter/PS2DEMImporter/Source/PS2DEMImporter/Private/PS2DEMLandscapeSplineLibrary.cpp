#include "PS2DEMLandscapeSplineLibrary.h"

#include "Components/SplineComponent.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IContentBrowserSingleton.h"
#include "Landscape.h"
#include "LandscapeEditLayer.h"
#include "LandscapeProxy.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "Misc/MessageDialog.h"
#include "ScopedTransaction.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "PS2DEMLandscapeSplineLibrary"

namespace
{
constexpr float SampleSpacingCm = 1000.0f;
constexpr float GroundOffsetCm = 0.0f;
const FName SplineLayerName(TEXT("PS2DEM_Splines"));
const FString GeneratedPrefix(TEXT("PS2DEM_"));

struct FRouteConfig
{
    FString Type;
    float WidthM = 0.0f;
    bool bRaiseTerrain = true;
    bool bLowerTerrain = true;
};

void ShowMessage(const FText& Title, const FText& Message)
{
    FMessageDialog::Open(EAppMsgType::Ok, Message, Title);
}

bool IsGenerated(const UObject* Object)
{
    return Object && Object->GetName().StartsWith(GeneratedPrefix);
}

ALandscape* RootLandscape(ALandscapeProxy* Proxy)
{
    if (!Proxy)
    {
        return nullptr;
    }
    if (ALandscape* Landscape = Cast<ALandscape>(Proxy))
    {
        return Landscape;
    }
    return Proxy->GetLandscapeActor();
}

ALandscape* FindTargetLandscape(UWorld* World, FString& OutError)
{
    TSet<ALandscape*> SelectedLandscapes;
    if (GEditor)
    {
        for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
        {
            if (ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(*It))
            {
                if (ALandscape* Landscape = RootLandscape(Proxy))
                {
                    SelectedLandscapes.Add(Landscape);
                }
            }
        }
    }

    if (SelectedLandscapes.Num() == 1)
    {
        return SelectedLandscapes.Array()[0];
    }
    if (SelectedLandscapes.Num() > 1)
    {
        OutError = TEXT("More than one Landscape is selected. Select only one target Landscape.");
        return nullptr;
    }

    TArray<ALandscape*> Landscapes;
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        Landscapes.Add(*It);
    }
    if (Landscapes.Num() == 1)
    {
        return Landscapes[0];
    }
    OutError = Landscapes.IsEmpty()
        ? TEXT("No Landscape was found in the current level.")
        : TEXT("This level has multiple Landscapes. Select the target Landscape together with the route actors.");
    return nullptr;
}

bool RouteConfigFromActor(AActor* Actor, FRouteConfig& OutConfig)
{
    if (!Actor)
    {
        return false;
    }

    if (Actor->Tags.Contains(FName(TEXT("MainRoad"))))
    {
        OutConfig = {TEXT("MainRoad"), 6.0f, true, true};
    }
    else if (Actor->Tags.Contains(FName(TEXT("BranchRoad"))))
    {
        OutConfig = {TEXT("BranchRoad"), 3.5f, true, true};
    }
    else if (Actor->Tags.Contains(FName(TEXT("River"))))
    {
        OutConfig = {TEXT("River"), 12.0f, false, true};
    }
    else
    {
        return false;
    }

    for (const FName& Tag : Actor->Tags)
    {
        FString TagString = Tag.ToString();
        if (TagString.RemoveFromStart(TEXT("WIDTH_M_")))
        {
            const float ParsedWidth = FCString::Atof(*TagString);
            if (ParsedWidth > 0.0f)
            {
                OutConfig.WidthM = ParsedWidth;
            }
            break;
        }
    }
    return true;
}

struct FSelectedRoute
{
    AActor* Actor = nullptr;
    USplineComponent* Spline = nullptr;
    FRouteConfig Config;
};

TArray<FSelectedRoute> GetSelectedRoutes()
{
    TArray<FSelectedRoute> Routes;
    if (!GEditor)
    {
        return Routes;
    }

    for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
    {
        AActor* Actor = Cast<AActor>(*It);
        FRouteConfig Config;
        if (!RouteConfigFromActor(Actor, Config))
        {
            continue;
        }
        if (USplineComponent* Spline = Actor->FindComponentByClass<USplineComponent>())
        {
            if (Spline->GetNumberOfSplinePoints() >= 2)
            {
                Routes.Add({Actor, Spline, Config});
            }
        }
    }
    return Routes;
}

bool TraceTargetLandscape(
    UWorld* World,
    ALandscape* Target,
    const FVector2D& XY,
    FVector& OutWorldLocation)
{
    TArray<FHitResult> Hits;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(PS2DEMLandscapeSplineGround), true);
    const FVector Start(XY.X, XY.Y, 1000000.0);
    const FVector End(XY.X, XY.Y, -1000000.0);
    if (!World->LineTraceMultiByChannel(Hits, Start, End, ECC_Visibility, Params))
    {
        return false;
    }
    for (const FHitResult& Hit : Hits)
    {
        if (ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Hit.GetActor()))
        {
            if (RootLandscape(Proxy) == Target)
            {
                OutWorldLocation = Hit.ImpactPoint + FVector(0.0, 0.0, GroundOffsetCm);
                return true;
            }
        }
    }
    return false;
}

ULandscapeEditLayerSplines* GetOrCreateSplineLayer(ALandscape* Landscape, FString& OutError)
{
    if (ULandscapeEditLayerSplines* Existing = Cast<ULandscapeEditLayerSplines>(
        Landscape->FindEditLayerOfType(ULandscapeEditLayerSplines::StaticClass())))
    {
        return Existing;
    }

    if (ULandscapeEditLayerBase* Conflicting = Landscape->GetEditLayer(SplineLayerName))
    {
        const int32 ConflictingIndex = Landscape->GetLayerIndex(SplineLayerName);
        if (ConflictingIndex <= 0 || !Landscape->DeleteLayer(ConflictingIndex))
        {
            OutError = FString::Printf(
                TEXT("The legacy regular Edit Layer '%s' could not be removed. Delete it in Landscape Mode, then run the command again."),
                *SplineLayerName.ToString());
            return nullptr;
        }
        Landscape->ForceLayersFullUpdate();
        UE_LOG(LogTemp, Display,
            TEXT("PS2DEM: migrated legacy regular Edit Layer '%s' to a Spline Edit Layer."),
            *SplineLayerName.ToString());
    }

    const int32 LayerIndex = Landscape->CreateLayer(
        SplineLayerName, ULandscapeEditLayerSplines::StaticClass());
    if (LayerIndex == INDEX_NONE)
    {
        OutError = TEXT("UE could not create the Landscape Spline Edit Layer.");
        return nullptr;
    }
    Landscape->SetSelectedEditLayerIndex(LayerIndex);
    return Cast<ULandscapeEditLayerSplines>(Landscape->GetEditLayer(LayerIndex));
}

int32 RemoveGenerated(ULandscapeSplinesComponent* Splines)
{
    if (!Splines)
    {
        return 0;
    }

    Splines->Modify();
    int32 Removed = 0;
    const TArray<TObjectPtr<ULandscapeSplineSegment>> SegmentsCopy = Splines->GetSegments();
    for (ULandscapeSplineSegment* Segment : SegmentsCopy)
    {
        if (!IsGenerated(Segment))
        {
            continue;
        }
        Segment->Modify();
        ULandscapeSplineControlPoint* Start = Segment->Connections[0].ControlPoint;
        ULandscapeSplineControlPoint* End = Segment->Connections[1].ControlPoint;
        if (Start)
        {
            Start->Modify();
            Start->ConnectedSegments.Remove(FLandscapeSplineConnection(Segment, 0));
        }
        if (End)
        {
            End->Modify();
            End->ConnectedSegments.Remove(FLandscapeSplineConnection(Segment, 1));
        }
        Splines->GetSegments().Remove(Segment);
        Segment->DeleteSplinePoints();
        ++Removed;
    }

    const TArray<TObjectPtr<ULandscapeSplineControlPoint>> PointsCopy = Splines->GetControlPoints();
    for (ULandscapeSplineControlPoint* Point : PointsCopy)
    {
        if (!IsGenerated(Point))
        {
            continue;
        }
        Point->Modify();
        Point->ConnectedSegments.Reset();
        Splines->GetControlPoints().Remove(Point);
        Point->DeleteSplinePoints();
    }
    return Removed;
}

TArray<FVector> SampleRouteOnLandscape(
    const FSelectedRoute& Route,
    ALandscape* Landscape,
    ULandscapeSplinesComponent* Splines,
    int32& OutMisses)
{
    TArray<FVector> LocalPoints;
    OutMisses = 0;
    const float Length = Route.Spline->GetSplineLength();
    const bool bClosed = Route.Spline->IsClosedLoop();
    const int32 SectionCount = FMath::Max(1, FMath::CeilToInt(Length / SampleSpacingCm));
    const int32 PointCount = bClosed ? FMath::Max(3, SectionCount) : SectionCount + 1;
    const int32 Denominator = bClosed ? PointCount : PointCount - 1;

    for (int32 Index = 0; Index < PointCount; ++Index)
    {
        const float Distance = Length * static_cast<float>(Index) / static_cast<float>(Denominator);
        const FVector Source = Route.Spline->GetLocationAtDistanceAlongSpline(
            Distance, ESplineCoordinateSpace::World);
        FVector Grounded;
        if (!TraceTargetLandscape(
            Route.Actor->GetWorld(), Landscape, FVector2D(Source.X, Source.Y), Grounded))
        {
            ++OutMisses;
            continue;
        }
        LocalPoints.Add(Splines->GetComponentTransform().InverseTransformPosition(Grounded));
    }
    return LocalPoints;
}

int32 CreateLandscapeRoute(
    const FSelectedRoute& Route,
    int32 RouteIndex,
    ALandscape* Landscape,
    ULandscapeSplinesComponent* Splines,
    int32& OutMisses)
{
    const TArray<FVector> Points = SampleRouteOnLandscape(
        Route, Landscape, Splines, OutMisses);
    if (Points.Num() < 2)
    {
        return 0;
    }

    TArray<ULandscapeSplineControlPoint*> ControlPoints;
    const FString BaseName = FString::Printf(
        TEXT("PS2DEM_%s_R%03d"), *Route.Config.Type, RouteIndex);
    const float HalfWidthCm = Route.Config.WidthM * 50.0f;
    const float FalloffCm = FMath::Max(200.0f, HalfWidthCm);

    for (int32 Index = 0; Index < Points.Num(); ++Index)
    {
        const FName DesiredName(*FString::Printf(TEXT("%s_CP%04d"), *BaseName, Index));
        ULandscapeSplineControlPoint* Point = NewObject<ULandscapeSplineControlPoint>(
            Splines,
            MakeUniqueObjectName(Splines, ULandscapeSplineControlPoint::StaticClass(), DesiredName),
            RF_Transactional);
        Point->Location = Points[Index];
        FVector Direction;
        if (Route.Spline->IsClosedLoop())
        {
            Direction = Points[(Index + 1) % Points.Num()] - Points[(Index - 1 + Points.Num()) % Points.Num()];
        }
        else if (Index == 0)
        {
            Direction = Points[1] - Points[0];
        }
        else if (Index == Points.Num() - 1)
        {
            Direction = Points[Index] - Points[Index - 1];
        }
        else
        {
            Direction = Points[Index + 1] - Points[Index - 1];
        }
        Point->Rotation = Direction.Rotation();
        Point->Width = HalfWidthCm;
        Point->SideFalloff = FalloffCm;
        Point->EndFalloff = FalloffCm;
        Point->LayerWidthRatio = 1.0f;
        Point->bRaiseTerrain = Route.Config.bRaiseTerrain;
        Point->bLowerTerrain = Route.Config.bLowerTerrain;
        Point->SegmentMeshOffset = 0.0f;
        Splines->GetControlPoints().Add(Point);
        ControlPoints.Add(Point);
    }

    const int32 SegmentCount = Route.Spline->IsClosedLoop()
        ? ControlPoints.Num()
        : ControlPoints.Num() - 1;
    for (int32 Index = 0; Index < SegmentCount; ++Index)
    {
        ULandscapeSplineControlPoint* Start = ControlPoints[Index];
        ULandscapeSplineControlPoint* End = ControlPoints[(Index + 1) % ControlPoints.Num()];
        const FName DesiredName(*FString::Printf(TEXT("%s_SEG%04d"), *BaseName, Index));
        ULandscapeSplineSegment* Segment = NewObject<ULandscapeSplineSegment>(
            Splines,
            MakeUniqueObjectName(Splines, ULandscapeSplineSegment::StaticClass(), DesiredName),
            RF_Transactional);
        Segment->Connections[0].ControlPoint = Start;
        Segment->Connections[1].ControlPoint = End;
        Segment->Connections[0].SocketName = NAME_None;
        Segment->Connections[1].SocketName = NAME_None;
        const float TangentLength = FVector::Distance(Start->Location, End->Location);
        Segment->Connections[0].TangentLen = TangentLength;
        Segment->Connections[1].TangentLen = TangentLength;
        Segment->bRaiseTerrain = Route.Config.bRaiseTerrain;
        Segment->bLowerTerrain = Route.Config.bLowerTerrain;
        Segment->LayerName = NAME_None;
        Segment->bHiddenInGame = false;
        Segment->bCastShadow = true;
        Splines->GetSegments().Add(Segment);
        Start->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 0));
        End->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 1));
        Segment->AutoFlipTangents();
    }
    return SegmentCount;
}

UStaticMesh* GetSelectedStaticMesh(FString& OutError)
{
    FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(
        TEXT("ContentBrowser"));
    TArray<FAssetData> Assets;
    ContentBrowser.Get().GetSelectedAssets(Assets);
    TArray<UStaticMesh*> Meshes;
    for (const FAssetData& Asset : Assets)
    {
        if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset()))
        {
            Meshes.Add(Mesh);
        }
    }
    if (Meshes.Num() != 1)
    {
        OutError = TEXT("Select exactly one Static Mesh in the Content Browser, then run the command again.");
        return nullptr;
    }
    return Meshes[0];
}

bool SegmentMatchesType(const ULandscapeSplineSegment* Segment, const FString& RouteType)
{
    return IsGenerated(Segment)
        && Segment->GetName().StartsWith(FString::Printf(TEXT("PS2DEM_%s_"), *RouteType));
}
}

bool UPS2DEMLandscapeSplineLibrary::ConvertSelectedRoutesToLandscapeSplines(bool bShowConfirmation)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        ShowMessage(LOCTEXT("ConvertFailedTitle", "PS2DEM Landscape Spline Import Failed"),
            LOCTEXT("NoWorld", "No editor world is active."));
        return false;
    }

    const TArray<FSelectedRoute> Routes = GetSelectedRoutes();
    if (Routes.IsEmpty())
    {
        ShowMessage(LOCTEXT("ConvertFailedTitle", "PS2DEM Landscape Spline Import Failed"),
            LOCTEXT("NoRoutes", "Select one or more imported MainRoad, BranchRoad or River actors."));
        return false;
    }

    FString Error;
    ALandscape* Landscape = FindTargetLandscape(World, Error);
    if (!Landscape)
    {
        ShowMessage(LOCTEXT("ConvertFailedTitle", "PS2DEM Landscape Spline Import Failed"), FText::FromString(Error));
        return false;
    }

    if (bShowConfirmation && FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::Format(
            LOCTEXT("ConvertConfirm", "Convert {0} selected route(s) into persistent Landscape Splines on '{1}'?\n\nExisting PS2DEM-generated Landscape Spline points and segments will be replaced. If the old regular 'PS2DEM_Splines' deformation layer exists, it will be removed and recreated as a Spline Edit Layer. Hand-made Landscape Splines will be preserved."),
            FText::AsNumber(Routes.Num()),
            FText::FromString(Landscape->GetActorLabel())),
        LOCTEXT("ConvertTitle", "Convert to Landscape Splines")) != EAppReturnType::Yes)
    {
        return false;
    }

    const FScopedTransaction Transaction(LOCTEXT("ConvertTransaction", "Convert PS2DEM Routes to Landscape Splines"));
    Landscape->Modify();
    ULandscapeEditLayerSplines* SplineLayer = GetOrCreateSplineLayer(Landscape, Error);
    if (!SplineLayer)
    {
        ShowMessage(LOCTEXT("ConvertFailedTitle", "PS2DEM Landscape Spline Import Failed"), FText::FromString(Error));
        return false;
    }

    if (!Landscape->GetSplinesComponent())
    {
        Landscape->CreateSplineComponent();
    }
    ULandscapeSplinesComponent* Splines = Landscape->GetSplinesComponent();
    if (!Splines)
    {
        ShowMessage(LOCTEXT("ConvertFailedTitle", "PS2DEM Landscape Spline Import Failed"),
            LOCTEXT("NoSplineComponent", "UE could not create the Landscape Splines component."));
        return false;
    }

    const int32 Removed = RemoveGenerated(Splines);
    int32 CreatedSegments = 0;
    int32 SurfaceMisses = 0;
    for (int32 Index = 0; Index < Routes.Num(); ++Index)
    {
        int32 RouteMisses = 0;
        CreatedSegments += CreateLandscapeRoute(
            Routes[Index], Index + 1, Landscape, Splines, RouteMisses);
        SurfaceMisses += RouteMisses;
    }

    Splines->RebuildAllSplines(true);
    Splines->MarkRenderStateDirty();
    Splines->MarkPackageDirty();
    Landscape->RequestSplineLayerUpdate();
    Landscape->RequestLayersContentUpdateForceAll();
    Landscape->MarkPackageDirty();

    const FText Completion = FText::Format(
        LOCTEXT("ConvertDone", "Created {0} persistent Landscape Spline segment(s) from {1} route(s).\nRemoved {2} previously generated segment(s).\nGround trace misses: {3}.\n\nThe level was not saved automatically. Open Landscape Mode > Manage > Splines to edit segments or assign meshes."),
        FText::AsNumber(CreatedSegments),
        FText::AsNumber(Routes.Num()),
        FText::AsNumber(Removed),
        FText::AsNumber(SurfaceMisses));
    UE_LOG(LogTemp, Display, TEXT("PS2DEM: %s"), *Completion.ToString());
    if (bShowConfirmation)
    {
        ShowMessage(LOCTEXT("ConvertDoneTitle", "PS2DEM Landscape Spline Import"), Completion);
    }
    return true;
}

void UPS2DEMLandscapeSplineLibrary::AssignSelectedMeshToGeneratedLandscapeSplines(
    const FString& RouteType)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    FString Error;
    ALandscape* Landscape = World ? FindTargetLandscape(World, Error) : nullptr;
    if (!Landscape || !Landscape->GetSplinesComponent())
    {
        ShowMessage(LOCTEXT("MeshFailedTitle", "PS2DEM Landscape Spline Mesh Failed"),
            FText::FromString(Error.IsEmpty() ? TEXT("No generated Landscape Splines were found.") : Error));
        return;
    }

    UStaticMesh* Mesh = GetSelectedStaticMesh(Error);
    if (!Mesh)
    {
        ShowMessage(LOCTEXT("MeshFailedTitle", "PS2DEM Landscape Spline Mesh Failed"), FText::FromString(Error));
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("AssignMeshTransaction", "Assign PS2DEM Landscape Spline Mesh"));
    ULandscapeSplinesComponent* Splines = Landscape->GetSplinesComponent();
    Splines->Modify();
    int32 Updated = 0;
    for (ULandscapeSplineSegment* Segment : Splines->GetSegments())
    {
        if (!SegmentMatchesType(Segment, RouteType))
        {
            continue;
        }
        Segment->Modify();
        FLandscapeSplineMeshEntry Entry;
        Entry.Mesh = Mesh;
        Entry.bCenterH = true;
        Entry.bScaleToWidth = true;
        Entry.bNoZScaling = true;
        Entry.ForwardAxis = ESplineMeshAxis::X;
        Entry.UpAxis = ESplineMeshAxis::Z;
        Segment->SplineMeshes.Reset();
        Segment->SplineMeshes.Add(Entry);
        ++Updated;
    }
    Splines->RebuildAllSplines(true);
    Splines->MarkPackageDirty();
    Landscape->RequestSplineLayerUpdate();
    Landscape->MarkPackageDirty();

    ShowMessage(
        LOCTEXT("MeshDoneTitle", "PS2DEM Landscape Spline Mesh"),
        FText::Format(
            LOCTEXT("MeshDone", "Assigned '{0}' to {1} {2} segment(s).\n\nThe level was not saved automatically."),
            FText::FromString(Mesh->GetName()),
            FText::AsNumber(Updated),
            FText::FromString(RouteType)));
}

FString UPS2DEMLandscapeSplineLibrary::GetGeneratedLandscapeSplineSummary()
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    FString Error;
    ALandscape* Landscape = World ? FindTargetLandscape(World, Error) : nullptr;
    if (!Landscape)
    {
        return Error.IsEmpty() ? TEXT("No editor Landscape was found.") : Error;
    }

    const bool bHasSplineLayer = Landscape->FindEditLayerOfType(
        ULandscapeEditLayerSplines::StaticClass()) != nullptr;
    int32 GeneratedPoints = 0;
    int32 GeneratedSegments = 0;
    if (ULandscapeSplinesComponent* Splines = Landscape->GetSplinesComponent())
    {
        for (const ULandscapeSplineControlPoint* Point : Splines->GetControlPoints())
        {
            GeneratedPoints += IsGenerated(Point) ? 1 : 0;
        }
        for (const ULandscapeSplineSegment* Segment : Splines->GetSegments())
        {
            GeneratedSegments += IsGenerated(Segment) ? 1 : 0;
        }
    }

    return FString::Printf(
        TEXT("Landscape=%s; spline_layer=%s; generated_points=%d; generated_segments=%d"),
        *Landscape->GetActorLabel(),
        bHasSplineLayer ? TEXT("true") : TEXT("false"),
        GeneratedPoints,
        GeneratedSegments);
}

void UPS2DEMLandscapeSplineLibrary::RemoveGeneratedLandscapeSplines()
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    FString Error;
    ALandscape* Landscape = World ? FindTargetLandscape(World, Error) : nullptr;
    if (!Landscape || !Landscape->GetSplinesComponent())
    {
        ShowMessage(LOCTEXT("RemoveFailedTitle", "Remove PS2DEM Landscape Splines Failed"),
            FText::FromString(Error.IsEmpty() ? TEXT("No Landscape Splines were found.") : Error));
        return;
    }
    if (FMessageDialog::Open(
        EAppMsgType::YesNo,
        LOCTEXT("RemoveConfirm", "Remove all Landscape Spline points and segments generated by PS2DEM?\n\nHand-made Landscape Splines will be preserved."),
        LOCTEXT("RemoveTitle", "Remove PS2DEM Landscape Splines")) != EAppReturnType::Yes)
    {
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("RemoveTransaction", "Remove PS2DEM Landscape Splines"));
    ULandscapeSplinesComponent* Splines = Landscape->GetSplinesComponent();
    const int32 Removed = RemoveGenerated(Splines);
    Splines->RebuildAllSplines(true);
    Splines->MarkPackageDirty();
    Landscape->RequestSplineLayerUpdate();
    Landscape->RequestLayersContentUpdateForceAll();
    Landscape->MarkPackageDirty();
    ShowMessage(
        LOCTEXT("RemoveDoneTitle", "Remove PS2DEM Landscape Splines"),
        FText::Format(LOCTEXT("RemoveDone", "Removed {0} generated segment(s)."), FText::AsNumber(Removed)));
}

#undef LOCTEXT_NAMESPACE
