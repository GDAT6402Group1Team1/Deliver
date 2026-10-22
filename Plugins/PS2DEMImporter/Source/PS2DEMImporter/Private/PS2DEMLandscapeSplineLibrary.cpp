#include "PS2DEMLandscapeSplineLibrary.h"

#include "Components/SplineComponent.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/World.h"
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
#include "GameFramework/Actor.h"
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

bool IsGeneratedRouteType(const UObject* Object, const FString& RouteType)
{
    return IsGenerated(Object)
        && (RouteType.IsEmpty()
            || Object->GetName().StartsWith(
                FString::Printf(TEXT("PS2DEM_%s_"), *RouteType)));
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
        OutConfig = {TEXT("MainRoad"), 18.0f, true, true};
    }
    else if (Actor->Tags.Contains(FName(TEXT("BranchRoad"))))
    {
        OutConfig = {TEXT("BranchRoad"), 10.8f, true, true};
    }
    else if (Actor->Tags.Contains(FName(TEXT("River"))))
    {
        OutConfig = {TEXT("River"), 16.0f, false, true};
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

TArray<FSelectedRoute> GetSelectedRoutes(const FString& RequiredRouteType = FString())
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
        if (!RequiredRouteType.IsEmpty() && Config.Type != RequiredRouteType)
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

int32 RemoveGenerated(
    ULandscapeSplinesComponent* Splines,
    const FString& RequiredRouteType = FString())
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
        if (!IsGeneratedRouteType(Segment, RequiredRouteType))
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
        if (!IsGeneratedRouteType(Point, RequiredRouteType))
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

struct FIndexedRoadSegment
{
    int32 Index = INDEX_NONE;
    ULandscapeSplineSegment* Segment = nullptr;
};

struct FGeneratedRoadGroup
{
    FString BaseName;
    FString Type;
    TArray<FIndexedRoadSegment> Segments;
};

struct FOrientedRoadSegment
{
    ULandscapeSplineSegment* Segment = nullptr;
    bool bForward = true;

    const FLandscapeSplineSegmentConnection& Start() const
    {
        return Segment->Connections[bForward ? 0 : 1];
    }

    const FLandscapeSplineSegmentConnection& End() const
    {
        return Segment->Connections[bForward ? 1 : 0];
    }
};

struct FActorRoadPoint
{
    FVector WorldLocation = FVector::ZeroVector;
    FVector ArriveTangent = FVector::ZeroVector;
    FVector LeaveTangent = FVector::ZeroVector;
    float WidthM = 0.0f;
};

struct FActorRoadRoute
{
    FString BaseName;
    FString Type;
    bool bClosed = false;
    float WidthM = 0.0f;
    TArray<FActorRoadPoint> Points;
};

bool ParseGeneratedRoadSegment(
    ULandscapeSplineSegment* Segment,
    FString& OutBaseName,
    FString& OutType,
    int32& OutSegmentIndex)
{
    if (!Segment)
    {
        return false;
    }

    const FString Name = Segment->GetName();
    static const TArray<FString> RoadTypes = {TEXT("MainRoad"), TEXT("BranchRoad")};
    for (const FString& Type : RoadTypes)
    {
        const FString Prefix = FString::Printf(TEXT("PS2DEM_%s_"), *Type);
        if (!Name.StartsWith(Prefix))
        {
            continue;
        }

        const int32 Marker = Name.Find(TEXT("_SEG"), ESearchCase::CaseSensitive);
        if (Marker == INDEX_NONE || Marker <= Prefix.Len())
        {
            return false;
        }
        const FString RouteId = Name.Mid(Prefix.Len(), Marker - Prefix.Len());
        if (!RouteId.StartsWith(TEXT("R")))
        {
            return false;
        }

        OutBaseName = Name.Left(Marker);
        OutType = Type;
        OutSegmentIndex = FCString::Atoi(*Name.Mid(Marker + 4));
        return true;
    }
    return false;
}

void GetConnectionWorldData(
    const FLandscapeSplineSegmentConnection& Connection,
    bool bStart,
    const FTransform& ComponentTransform,
    FVector& OutLocation,
    FVector& OutTangent)
{
    FVector LocalLocation = FVector::ZeroVector;
    FRotator LocalRotation = FRotator::ZeroRotator;
    Connection.ControlPoint->GetConnectionLocationAndRotation(
        Connection.SocketName, LocalLocation, LocalRotation);
    const float SignedTangentLength = bStart
        ? Connection.TangentLen
        : -Connection.TangentLen;
    OutLocation = ComponentTransform.TransformPosition(LocalLocation);
    OutTangent = ComponentTransform.TransformVector(
        LocalRotation.Vector() * SignedTangentLength);
}

bool BuildActorRoadRoute(
    FGeneratedRoadGroup Group,
    const FTransform& ComponentTransform,
    FActorRoadRoute& OutRoute,
    FString& OutError)
{
    Group.Segments.Sort([](const FIndexedRoadSegment& A, const FIndexedRoadSegment& B)
    {
        return A.Index < B.Index;
    });
    if (Group.Segments.IsEmpty())
    {
        return false;
    }

    TMap<ULandscapeSplineControlPoint*, TArray<ULandscapeSplineSegment*>> Adjacency;
    for (const FIndexedRoadSegment& Indexed : Group.Segments)
    {
        ULandscapeSplineSegment* Segment = Indexed.Segment;
        if (!Segment || !Segment->Connections[0].ControlPoint || !Segment->Connections[1].ControlPoint)
        {
            OutError = FString::Printf(TEXT("%s contains an incomplete Landscape Spline segment."), *Group.BaseName);
            return false;
        }
        Adjacency.FindOrAdd(Segment->Connections[0].ControlPoint).Add(Segment);
        Adjacency.FindOrAdd(Segment->Connections[1].ControlPoint).Add(Segment);
    }

    TArray<ULandscapeSplineControlPoint*> Endpoints;
    for (const TPair<ULandscapeSplineControlPoint*, TArray<ULandscapeSplineSegment*>>& Pair : Adjacency)
    {
        if (Pair.Value.Num() == 1)
        {
            Endpoints.Add(Pair.Key);
        }
        else if (Pair.Value.Num() != 2)
        {
            OutError = FString::Printf(
                TEXT("%s contains a branch or disconnected Landscape Spline control point."),
                *Group.BaseName);
            return false;
        }
    }
    if (Endpoints.Num() != 0 && Endpoints.Num() != 2)
    {
        OutError = FString::Printf(
            TEXT("%s is not one continuous open or closed Landscape Spline route."),
            *Group.BaseName);
        return false;
    }

    ULandscapeSplineControlPoint* PreferredStart = Group.Segments[0].Segment->Connections[0].ControlPoint;
    ULandscapeSplineControlPoint* Current = PreferredStart;
    if (Endpoints.Num() == 2 && Adjacency.FindChecked(PreferredStart).Num() != 1)
    {
        Current = Endpoints[0];
    }

    TSet<ULandscapeSplineSegment*> Used;
    TArray<FOrientedRoadSegment> Ordered;
    Ordered.Reserve(Group.Segments.Num());
    while (Ordered.Num() < Group.Segments.Num())
    {
        ULandscapeSplineSegment* Next = nullptr;
        for (ULandscapeSplineSegment* Candidate : Adjacency.FindChecked(Current))
        {
            if (!Used.Contains(Candidate))
            {
                Next = Candidate;
                break;
            }
        }
        if (!Next)
        {
            OutError = FString::Printf(
                TEXT("%s contains disconnected Landscape Spline segments."),
                *Group.BaseName);
            return false;
        }

        const bool bForward = Next->Connections[0].ControlPoint == Current;
        Ordered.Add({Next, bForward});
        Used.Add(Next);
        Current = Next->Connections[bForward ? 1 : 0].ControlPoint;
    }

    OutRoute.BaseName = Group.BaseName;
    OutRoute.Type = Group.Type;
    OutRoute.bClosed = Endpoints.IsEmpty();
    if ((OutRoute.bClosed && Current != Ordered[0].Start().ControlPoint)
        || (!OutRoute.bClosed && !Endpoints.Contains(Current)))
    {
        OutError = FString::Printf(
            TEXT("%s could not be traversed as one complete Landscape Spline route."),
            *Group.BaseName);
        return false;
    }

    const int32 PointCount = OutRoute.bClosed
        ? Ordered.Num()
        : Ordered.Num() + 1;
    OutRoute.Points.Reserve(PointCount);
    double WidthSum = 0.0;

    for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
    {
        FActorRoadPoint Point;
        if (OutRoute.bClosed || PointIndex < Ordered.Num())
        {
            const int32 SegmentIndex = PointIndex % Ordered.Num();
            const int32 PreviousSegmentIndex = (SegmentIndex - 1 + Ordered.Num()) % Ordered.Num();
            const FOrientedRoadSegment& Segment = Ordered[SegmentIndex];
            FVector IgnoredLocation;
            GetConnectionWorldData(
                Segment.Start(), true, ComponentTransform,
                Point.WorldLocation, Point.LeaveTangent);
            if (OutRoute.bClosed || PointIndex > 0)
            {
                GetConnectionWorldData(
                    Ordered[PreviousSegmentIndex].End(), false,
                    ComponentTransform, IgnoredLocation, Point.ArriveTangent);
            }
            else
            {
                Point.ArriveTangent = Point.LeaveTangent;
            }
            Point.WidthM = Segment.Start().ControlPoint->Width * 0.02f;
        }
        else
        {
            const FOrientedRoadSegment& Segment = Ordered.Last();
            GetConnectionWorldData(
                Segment.End(), false, ComponentTransform,
                Point.WorldLocation, Point.ArriveTangent);
            Point.LeaveTangent = Point.ArriveTangent;
            Point.WidthM = Segment.End().ControlPoint->Width * 0.02f;
        }
        WidthSum += Point.WidthM;
        OutRoute.Points.Add(Point);
    }

    OutRoute.WidthM = static_cast<float>(WidthSum / FMath::Max(1, OutRoute.Points.Num()));
    return OutRoute.Points.Num() >= 2;
}

TArray<FActorRoadRoute> CollectGeneratedRoadRoutes(
    ULandscapeSplinesComponent* Splines,
    TArray<FString>& OutWarnings)
{
    TMap<FString, FGeneratedRoadGroup> Groups;
    for (ULandscapeSplineSegment* Segment : Splines->GetSegments())
    {
        FString BaseName;
        FString Type;
        int32 SegmentIndex = INDEX_NONE;
        if (!ParseGeneratedRoadSegment(Segment, BaseName, Type, SegmentIndex))
        {
            continue;
        }
        FGeneratedRoadGroup& Group = Groups.FindOrAdd(BaseName);
        Group.BaseName = BaseName;
        Group.Type = Type;
        Group.Segments.Add({SegmentIndex, Segment});
    }

    TArray<FString> Keys;
    Groups.GetKeys(Keys);
    Keys.Sort();
    TArray<FActorRoadRoute> Routes;
    Routes.Reserve(Keys.Num());
    const FTransform ComponentTransform = Splines->GetComponentTransform();
    for (const FString& Key : Keys)
    {
        const FGeneratedRoadGroup& SourceGroup = Groups[Key];
        TMap<ULandscapeSplineControlPoint*, TArray<FIndexedRoadSegment>> Adjacency;
        for (const FIndexedRoadSegment& Indexed : SourceGroup.Segments)
        {
            if (!Indexed.Segment
                || !Indexed.Segment->Connections[0].ControlPoint
                || !Indexed.Segment->Connections[1].ControlPoint)
            {
                continue;
            }
            Adjacency.FindOrAdd(Indexed.Segment->Connections[0].ControlPoint).Add(Indexed);
            Adjacency.FindOrAdd(Indexed.Segment->Connections[1].ControlPoint).Add(Indexed);
        }

        TSet<ULandscapeSplineSegment*> Visited;
        TArray<FGeneratedRoadGroup> Components;
        for (const FIndexedRoadSegment& Seed : SourceGroup.Segments)
        {
            if (!Seed.Segment || Visited.Contains(Seed.Segment))
            {
                continue;
            }

            FGeneratedRoadGroup Component;
            Component.BaseName = SourceGroup.BaseName;
            Component.Type = SourceGroup.Type;
            TArray<FIndexedRoadSegment> Queue = {Seed};
            Visited.Add(Seed.Segment);
            for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
            {
                const FIndexedRoadSegment Current = Queue[QueueIndex];
                Component.Segments.Add(Current);
                for (int32 EndIndex = 0; EndIndex < 2; ++EndIndex)
                {
                    ULandscapeSplineControlPoint* Point = Current.Segment->Connections[EndIndex].ControlPoint;
                    if (!Point)
                    {
                        continue;
                    }
                    for (const FIndexedRoadSegment& Neighbour : Adjacency.FindChecked(Point))
                    {
                        if (Neighbour.Segment && !Visited.Contains(Neighbour.Segment))
                        {
                            Visited.Add(Neighbour.Segment);
                            Queue.Add(Neighbour);
                        }
                    }
                }
            }
            Components.Add(MoveTemp(Component));
        }

        for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
        {
            if (Components.Num() > 1)
            {
                Components[ComponentIndex].BaseName += FString::Printf(
                    TEXT("_Part%02d"), ComponentIndex + 1);
            }
            FActorRoadRoute Route;
            FString Error;
            if (BuildActorRoadRoute(
                Components[ComponentIndex], ComponentTransform, Route, Error))
            {
                Routes.Add(MoveTemp(Route));
            }
            else if (!Error.IsEmpty())
            {
                OutWarnings.Add(Error);
            }
        }
    }
    return Routes;
}

bool IsGeneratedRoadActor(const AActor* Actor)
{
    return Actor
        && Actor->Tags.Contains(FName(TEXT("PS2DEM_SPLINE")))
        && (Actor->Tags.Contains(FName(TEXT("MainRoad")))
            || Actor->Tags.Contains(FName(TEXT("BranchRoad"))));
}

int32 RemoveGeneratedRoadActors(UWorld* World)
{
    TArray<AActor*> ActorsToRemove;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (IsGeneratedRoadActor(*It))
        {
            ActorsToRemove.Add(*It);
        }
    }
    for (AActor* Actor : ActorsToRemove)
    {
        World->EditorDestroyActor(Actor, true);
    }
    return ActorsToRemove.Num();
}

AActor* CreateRoadActor(UWorld* World, ULevel* Level, const FActorRoadRoute& Route)
{
    if (!World || Route.Points.Num() < 2)
    {
        return nullptr;
    }

    const FVector Origin = Route.Points[0].WorldLocation;

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Name = MakeUniqueObjectName(
        Level, AActor::StaticClass(),
        FName(*FString::Printf(TEXT("PS2DEM_Actor_%s"), *Route.BaseName)));
    SpawnParameters.OverrideLevel = Level;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    SpawnParameters.ObjectFlags |= RF_Transactional;
    AActor* Actor = World->SpawnActor<AActor>(
        AActor::StaticClass(), Route.Points[0].WorldLocation,
        FRotator::ZeroRotator, SpawnParameters);
    if (!Actor)
    {
        return nullptr;
    }

    Actor->Modify();
    Actor->SetActorLabel(Route.BaseName.Replace(TEXT("PS2DEM_"), TEXT("")), true);
    Actor->SetFolderPath(FName(*FString::Printf(TEXT("PS2DEM/Splines/%s"), *Route.Type)));
    Actor->Tags = {
        FName(TEXT("PS2DEM_SPLINE")),
        FName(TEXT("PS2DEM_LANDSCAPE_EXPORT")),
        FName(*Route.Type),
        FName(*FString::Printf(TEXT("WIDTH_M_%s"), *FString::SanitizeFloat(Route.WidthM)))
    };

    USplineComponent* Spline = NewObject<USplineComponent>(
        Actor, FName(TEXT("Spline")), RF_Transactional);
    Actor->AddInstanceComponent(Spline);
    Actor->SetRootComponent(Spline);
    Spline->OnComponentCreated();
    Spline->RegisterComponent();
    Spline->SetRelativeTransform(FTransform::Identity);
    Spline->Modify();
    Spline->bInputSplinePointsToConstructionScript = false;
    Spline->bSplineHasBeenEdited = true;
    Spline->bAllowDiscontinuousSpline = true;
#if WITH_EDITORONLY_DATA
    const FLinearColor Color = Route.Type == TEXT("MainRoad")
        ? FLinearColor(1.0f, 0.32f, 0.02f, 1.0f)
        : FLinearColor(1.0f, 0.9f, 0.25f, 1.0f);
    Spline->EditorUnselectedSplineSegmentColor = Color;
    Spline->EditorSelectedSplineSegmentColor = Color;
#endif

    // AActor has no root component when it is spawned. Registering a new root and
    // assigning an identity relative transform therefore resets its world transform
    // to the world origin. Restore the intended actor origin before adding local
    // spline points, otherwise every route gets an unwanted segment from (0,0,0).
    Actor->SetActorLocation(Origin, false, nullptr, ETeleportType::TeleportPhysics);
    Spline->ClearSplinePoints(false);
    for (int32 Index = 0; Index < Route.Points.Num(); ++Index)
    {
        const FActorRoadPoint& Point = Route.Points[Index];
        Spline->AddSplinePoint(
            Point.WorldLocation - Origin, ESplineCoordinateSpace::Local, false);
        Spline->SetSplinePointType(Index, ESplinePointType::CurveCustomTangent, false);
        Spline->SetTangentsAtSplinePoint(
            Index, Point.ArriveTangent, Point.LeaveTangent,
            ESplineCoordinateSpace::Local, false);
        Spline->SetScaleAtSplinePoint(
            Index, FVector(1.0f, Point.WidthM, 1.0f), false);
    }
    Spline->SetClosedLoop(Route.bClosed, false);
    Spline->UpdateSpline();
    Actor->MarkPackageDirty();
    return Actor;
}

bool ExportRoadActorSplinesFromLandscapeImpl(bool bShowConfirmation)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    FString Error;
    ALandscape* Landscape = World ? FindTargetLandscape(World, Error) : nullptr;
    if (!Landscape || !Landscape->GetSplinesComponent())
    {
        ShowMessage(
            LOCTEXT("RoadExportFailedTitle", "PS2DEM Road Actor Export Failed"),
            FText::FromString(Error.IsEmpty()
                ? TEXT("No generated Landscape road splines were found.")
                : Error));
        return false;
    }

    TArray<FString> Warnings;
    const TArray<FActorRoadRoute> Routes = CollectGeneratedRoadRoutes(
        Landscape->GetSplinesComponent(), Warnings);
    if (Routes.IsEmpty())
    {
        FString Message = TEXT("No generated PS2DEM MainRoad or BranchRoad Landscape Splines were found.");
        if (!Warnings.IsEmpty())
        {
            Message += TEXT("\n\n") + FString::Join(Warnings, TEXT("\n"));
        }
        ShowMessage(
            LOCTEXT("RoadExportFailedTitle", "PS2DEM Road Actor Export Failed"),
            FText::FromString(Message));
        return false;
    }

    if (bShowConfirmation && FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::Format(
            LOCTEXT("RoadExportConfirm", "Rebuild {0} ordinary road Actor spline(s) from Landscape '{1}'?\n\nExisting PS2DEM MainRoad and BranchRoad actors will be replaced. River actors, River Landscape Splines and hand-made Landscape Splines will not be changed."),
            FText::AsNumber(Routes.Num()),
            FText::FromString(Landscape->GetActorLabel())),
        LOCTEXT("RoadExportTitle", "Export Road Actor Splines from Landscape")) != EAppReturnType::Yes)
    {
        return false;
    }

    const FScopedTransaction Transaction(
        LOCTEXT("RoadExportTransaction", "Export PS2DEM Road Actor Splines from Landscape"));
    const int32 Removed = RemoveGeneratedRoadActors(World);
    TArray<AActor*> CreatedActors;
    for (const FActorRoadRoute& Route : Routes)
    {
        if (AActor* Actor = CreateRoadActor(World, Landscape->GetLevel(), Route))
        {
            CreatedActors.Add(Actor);
        }
        else
        {
            Warnings.Add(FString::Printf(TEXT("Could not create Actor for %s."), *Route.BaseName));
        }
    }

    if (GEditor && !CreatedActors.IsEmpty())
    {
        GEditor->SelectNone(false, true, false);
        for (AActor* Actor : CreatedActors)
        {
            GEditor->SelectActor(Actor, true, false);
        }
        GEditor->NoteSelectionChange();
    }

    FString Message = FString::Printf(
        TEXT("Created %d road Actor spline(s); replaced %d previous road Actor(s).\n\nRivers were not exported or changed. The level was not saved automatically."),
        CreatedActors.Num(), Removed);
    if (!Warnings.IsEmpty())
    {
        Message += TEXT("\n\nWarnings:\n") + FString::Join(Warnings, TEXT("\n"));
    }
    UE_LOG(LogTemp, Display, TEXT("PS2DEM: %s"), *Message);
    if (bShowConfirmation)
    {
        ShowMessage(
            LOCTEXT("RoadExportDoneTitle", "PS2DEM Road Actor Export"),
            FText::FromString(Message));
    }
    return CreatedActors.Num() == Routes.Num();
}

bool ConvertSelectedRoutesToLandscapeSplinesImpl(
    bool bShowConfirmation,
    const FString& RequiredRouteType)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        ShowMessage(LOCTEXT("ConvertFailedTitle", "PS2DEM Landscape Spline Import Failed"),
            LOCTEXT("NoWorld", "No editor world is active."));
        return false;
    }

    const TArray<FSelectedRoute> Routes = GetSelectedRoutes(RequiredRouteType);
    if (Routes.IsEmpty())
    {
        const FText NoRoutesMessage = RequiredRouteType.IsEmpty()
            ? LOCTEXT("NoRoutes", "Select one or more imported MainRoad, BranchRoad or River actors.")
            : LOCTEXT("NoRiverRoutes", "Select one or more imported River actors.");
        ShowMessage(LOCTEXT("ConvertFailedTitle", "PS2DEM Landscape Spline Import Failed"),
            NoRoutesMessage);
        return false;
    }

    FString Error;
    ALandscape* Landscape = FindTargetLandscape(World, Error);
    if (!Landscape)
    {
        ShowMessage(LOCTEXT("ConvertFailedTitle", "PS2DEM Landscape Spline Import Failed"), FText::FromString(Error));
        return false;
    }

    const FText ConfirmationMessage = RequiredRouteType.IsEmpty()
        ? FText::Format(
            LOCTEXT("ConvertConfirm", "Convert {0} selected route(s) into persistent Landscape Splines on '{1}'?\n\nExisting PS2DEM-generated Landscape Spline points and segments will be replaced. If the old regular 'PS2DEM_Splines' deformation layer exists, it will be removed and recreated as a Spline Edit Layer. Hand-made Landscape Splines will be preserved."),
            FText::AsNumber(Routes.Num()),
            FText::FromString(Landscape->GetActorLabel()))
        : FText::Format(
            LOCTEXT("ConvertRiverConfirm", "Convert {0} selected River route(s) into persistent Landscape Splines on '{1}'?\n\nOnly PS2DEM-generated River points and segments will be replaced. Generated roads and hand-made Landscape Splines will be preserved."),
            FText::AsNumber(Routes.Num()),
            FText::FromString(Landscape->GetActorLabel()));
    if (bShowConfirmation && FMessageDialog::Open(
        EAppMsgType::YesNo,
        ConfirmationMessage,
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

    const int32 Removed = RemoveGenerated(Splines, RequiredRouteType);
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
}

bool UPS2DEMLandscapeSplineLibrary::ConvertSelectedRoutesToLandscapeSplines(bool bShowConfirmation)
{
    return ConvertSelectedRoutesToLandscapeSplinesImpl(bShowConfirmation, FString());
}

bool UPS2DEMLandscapeSplineLibrary::ConvertSelectedRiverRoutesToLandscapeSplines(
    bool bShowConfirmation)
{
    return ConvertSelectedRoutesToLandscapeSplinesImpl(bShowConfirmation, TEXT("River"));
}

bool UPS2DEMLandscapeSplineLibrary::ExportRoadActorSplinesFromLandscape(
    bool bShowConfirmation)
{
    return ExportRoadActorSplinesFromLandscapeImpl(bShowConfirmation);
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
