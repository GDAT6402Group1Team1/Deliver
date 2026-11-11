#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "NPC/DeliveryPedestrian.h"
#include "NPC/DeliveryPedestrianRoute.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"

// 原地图双人 PIE：远端正常驾驶 RPC、真实车身扫掠，不直接给受击者调用 ReceiveImpact
class FDeliveryBikeImpactPIECommand : public IAutomationLatentCommand
{
public:
    explicit FDeliveryBikeImpactPIECommand(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        UWorld* Server = nullptr; UWorld* Client = nullptr;
        for (const auto& Context : GEngine->GetWorldContexts())
            if (Context.WorldType == EWorldType::PIE && Context.World())
            {
                if (Context.World()->GetNetMode() == NM_Client) Client = Context.World();
                else Server = Context.World();
            }
        if (bEnding) return !Server && !Client;
        if (!bRequested)
        {
            if (Server || Client) { Test->AddError(TEXT("请先结束 PIE")); return true; }
            if (!FEditorFileUtils::LoadMap(TEXT("/Game/Level/TestForCharacter"), false, false)) return true;
            Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage()));
            Settings->SetPlayNetMode(PIE_ListenServer); Settings->SetPlayNumberOfClients(2); Settings->SetRunUnderOneProcess(true);
            FRequestPlaySessionParams Params; Params.EditorPlaySettings = Settings.Get(); Params.bAllowOnlineSubsystem = false;
            GEditor->RequestPlaySession(Params); bRequested = true; Started = FPlatformTime::Seconds(); return false;
        }
        if (FPlatformTime::Seconds() - Started > 150) { Test->AddError(FString::Printf(TEXT("车撞双人测试超时 stage=%d"), Stage)); return End(); }
        if (!Server || !Client) return false;
        auto* PC = Client->GetFirstPlayerController();
        if (!PC || !PC->GetPawn()) return false;
        const float Now = Server->GetTimeSeconds();
        if (Now < NextTime) return false;
        if (Stage == 0)
        {
            ADeliveryCharacter* Driver = nullptr;
            for (FConstPlayerControllerIterator It = Server->GetPlayerControllerIterator(); It; ++It)
                if (!It->Get()->IsLocalController()) Driver = Cast<ADeliveryCharacter>(It->Get()->GetPawn());
            if (!Driver || Driver->IsIncapacitated()) return false;
            ADeliveryPedestrianRoute* Route = nullptr;
            for (TActorIterator<ADeliveryPedestrianRoute> It(Server); It; ++It)
                if (It->CrosswalkActor && It->bUseBoids && !It->Kinds.IsEmpty() && It->Kinds[0].HeightScale < .7f) { Route = *It; break; }
            if (!Test->TestNotNull(TEXT("正式小学生过街路线"), Route)) return End();
            Test->TestTrue(TEXT("正式路线已启用 Boids"), Route->bUseBoids);
            const float Distance = Route->GetRouteLength() * .5f;
            Direction = FVector::CrossProduct(Route->GetDirectionAtDistance(Distance), FVector::UpVector).GetSafeNormal2D();
            FActorSpawnParameters Spawn; Spawn.Owner = Route; Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Child = Server->SpawnActor<ADeliveryPedestrian>(ADeliveryPedestrian::StaticClass(),
                FTransform(FRotator::ZeroRotator, Route->GetLocationAtDistance(Distance), FVector(.6f)), Spawn);
            Child->BeginWalk(Route, Route->Kinds[0].Mesh.LoadSynchronous(), nullptr, Distance, true, 0.f, 150.f);
            Origin = Child->GetActorLocation();
            auto* Class = LoadClass<ADeliveryMotorbike>(nullptr, TEXT("/Game/Vehicle/Motorbike/BP_Motorbike.BP_Motorbike_C"));
            if (!Test->TestNotNull(TEXT("正式电瓶车蓝图"), Class)) return End();
            Bike = Server->SpawnActor<ADeliveryMotorbike>(Class, Origin - Direction * 500.f + FVector(0,0,80), Direction.Rotation(), Spawn);
            // 动态 Actor 名字在不同 PIE 世界不保证一致，用复制的所有者唯一关联测试对象
            Child->SetOwner(Bike.Get());
            Driver->SetActorLocation(Bike->GetActorLocation() + FVector(0,0,80), false, nullptr, ETeleportType::TeleportPhysics);
            if (!Test->TestTrue(TEXT("远端玩家上车"), Bike->TryEnter(Driver))) return End();
            GroupMember = Route->SpawnOnePedestrian(true);
            if (!Test->TestTrue(TEXT("真实路线生成群体成员"), GroupMember.IsValid())) return End();
            GroupMember->BeginWalk(Route, Route->Kinds[0].Mesh.LoadSynchronous(), nullptr,
                FMath::Max(Distance - 250.f, Route->GetAppearDistance() + 30.f), true, 130.f, 150.f);
            GroupMember->SetOwner(Driver);
            GroupStart = GroupMember->GetActorLocation();
            ++Stage; NextTime = Now + 2.f; return false;
        }
        auto* RemoteBike = Cast<ADeliveryMotorbike>(PC->GetPawn());
        if (!RemoteBike) return false;
        ADeliveryPedestrian* RemoteChild = nullptr;
        for (TActorIterator<ADeliveryPedestrian> It(Client); It; ++It)
            if (It->GetOwner() == RemoteBike) RemoteChild = *It;
        if (!RemoteChild) return false;
        if (Stage == 1)
        {
            auto* Body = RemoteChild->FindComponentByClass<UPoseableMeshComponent>();
            Test->TestTrue(TEXT("远端有正式小学生模型"), Body && Body->GetSkinnedAsset() != nullptr && Body->IsVisible());
            Test->TestTrue(TEXT("远端小学生缩放正确"), FMath::IsNearlyEqual(RemoteChild->GetActorScale3D().X, .6, .02));
            PC->SetControlRotation(Direction.Rotation());
            RemoteBike->MoveInputRaw = FVector2D(0,1);
            ++Stage; DriveStarted = Now;
        }
        if (Stage == 2)
        {
            if (!Child->IsKnockedDown())
            {
                if (Now - DriveStarted > 7.f) { Test->AddError(FString::Printf(TEXT("实际骑车未撞中 speed=%.1f distance=%.1f"), Bike->GetCurrentSpeed(), FVector::Dist2D(Bike->GetActorLocation(), Origin))); return End(); }
                return false;
            }
            Test->AddInfo(FString::Printf(TEXT("BikeImpactPIE hit speed=%.1f"), Bike->GetCurrentSpeed()));
            RemoteBike->MoveInputRaw = FVector2D::ZeroVector;
            ++Stage; NextTime = Now + .75f; return false;
        }
        if (Stage == 3)
        {
            const float Travel = FVector::Dist2D(Origin, Child->GetActorLocation());
            Test->AddInfo(FString::Printf(TEXT("BikeImpactPIE displacement=%.1f clientError=%.1f"), Travel, FVector::Dist(Child->GetActorLocation(), RemoteChild->GetActorLocation())));
            Test->TestTrue(TEXT("实际车撞明显击退"), Travel > 100.f);
            Test->TestTrue(TEXT("倒地状态复制到驾驶客户端"), RemoteChild->IsKnockedDown());
            Test->TestTrue(TEXT("两端击退位置一致"), FVector::Dist(Child->GetActorLocation(), RemoteChild->GetActorLocation()) < 20.f);
            Test->TestTrue(TEXT("远端身体明显倾倒"), RemoteChild->GetActorUpVector().Z < .5f);
            ADeliveryPedestrian* RemoteMember = nullptr;
            for (TActorIterator<ADeliveryPedestrian> It(Client); It; ++It)
                if (It->GetOwner() == RemoteBike->GetDriver()) RemoteMember = *It;
            if (Test->TestNotNull(TEXT("Boids 群体成员复制到远端"), RemoteMember) && GroupMember.IsValid())
            {
                Test->TestTrue(TEXT("群体成员在真实地图前进"), FVector::Dist2D(GroupStart, GroupMember->GetActorLocation()) > 40.f);
                Test->TestTrue(TEXT("Boids 位置在两端同步"), FVector::Dist(GroupMember->GetActorLocation(), RemoteMember->GetActorLocation()) < 35.f);
            }
            ++Stage; NextTime = Now + 5.f; return false;
        }
        Test->TestFalse(TEXT("服务器恢复完成"), Child->IsKnockedDown() || Child->IsStandingUp());
        Test->TestFalse(TEXT("客户端恢复完成"), RemoteChild->IsKnockedDown() || RemoteChild->IsStandingUp());
        Test->TestTrue(TEXT("恢复后双方站直"), Child->GetActorUpVector().Z > .95f && RemoteChild->GetActorUpVector().Z > .95f);
        return End();
    }
private:
    bool End() { GEditor->RequestEndPlayMap(); bEnding = true; return false; }
    FAutomationTestBase* Test;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TWeakObjectPtr<ADeliveryMotorbike> Bike;
    TWeakObjectPtr<ADeliveryPedestrian> Child;
    TWeakObjectPtr<ADeliveryPedestrian> GroupMember;
    FVector Origin, Direction, GroupStart;
    double Started = 0;
    float NextTime = 0, DriveStarted = 0;
    int Stage = 0;
    bool bRequested = false, bEnding = false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryBikeImpactPIETest, "Delivery.Vehicle.PedestrianImpactMultiplayer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryBikeImpactPIETest::RunTest(const FString&)
{
    if (!FApp::CanEverRender()) { AddWarning(TEXT("车撞双人回归需要有渲染器的真实 PIE")); return true; }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryBikeImpactPIECommand(this));
    return true;
}
#endif
