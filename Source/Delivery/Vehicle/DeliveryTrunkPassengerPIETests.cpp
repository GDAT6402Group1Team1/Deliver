#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Player/DeliveryPlayerController.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Interaction/DeliveryInteractionComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerState.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"

// 可见平地上的 PIE 网络世界；正式角色/车/控制器，抓取、E 请求和驾驶走正式入口
class FDeliveryTrunkPassengerPIECommand : public IAutomationLatentCommand
{
public:
    FDeliveryTrunkPassengerPIECommand(FAutomationTestBase* InTest,bool InClientLoads,int InPlayers,bool InStress=false)
        : Test(InTest),bClientLoads(InClientLoads),bStress(InStress),Players(InPlayers) {}
    bool Update() override
    {
        UWorld* Server=nullptr;
        TArray<UWorld*> Worlds;
        for (const auto& Context:GEngine->GetWorldContexts())
            if (Context.WorldType==EWorldType::PIE && Context.World())
            {
                Worlds.Add(Context.World());
                if (Context.World()->GetNetMode()!=NM_Client) Server=Context.World();
            }
        if (bEnding) return Worlds.IsEmpty();
        if (!bRequested)
        {
            if (!Worlds.IsEmpty()) { Test->AddError(TEXT("请先结束 PIE")); return true; }
            if (!FEditorFileUtils::LoadMap(TEXT("/Engine/Maps/Templates/Template_Default"),false,false)) return true;
            auto* EditorWorld=GEditor->GetEditorWorldContext().World();
            EditorWorld->GetWorldSettings()->DefaultGameMode=LoadClass<AGameModeBase>(nullptr,TEXT("/Game/Blueprint/GameMode/BP_DeliverGameMode.BP_DeliverGameMode_C"));
            auto* Floor=EditorWorld->SpawnActor<AStaticMeshActor>();
            Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
            Floor->SetActorScale3D(FVector(400,400,.4));
            Floor->SetActorLocation(FVector(0,0,-20));
            Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
            if (bStress)
            {
                Floor->SetActorRotation(FRotator(8,0,0));
                if (auto* FPS=IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
                { PreviousFPS=FPS->GetFloat(); FPS->Set(10.f,ECVF_SetByCode); }
            }
            Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(),GetTransientPackage()));
            Settings->SetPlayNetMode(PIE_ListenServer);
            Settings->SetPlayNumberOfClients(Players); Settings->SetRunUnderOneProcess(true);
            FRequestPlaySessionParams Params; Params.EditorPlaySettings=Settings.Get(); Params.bAllowOnlineSubsystem=false;
            GEditor->RequestPlaySession(Params); bRequested=true; Started=FPlatformTime::Seconds(); return false;
        }
        if (FPlatformTime::Seconds()-Started>240)
        { Test->AddError(FString::Printf(TEXT("Trunk PIE timeout stage=%d"),Stage)); return End(); }
        if (!Server || Worlds.Num()!=Players) return false;
        const float Now=Server->GetTimeSeconds();
        if (Now<NextTime) return false;
        if (Stage==0)
        {
            ADeliveryCharacter* Host=nullptr; ADeliveryCharacter* Remote=nullptr;
            int Count=0;
            for (FConstPlayerControllerIterator It=Server->GetPlayerControllerIterator();It;++It)
            {
                auto* C=Cast<ADeliveryCharacter>(It->Get()->GetPawn());
                if (!C || !C->GetPlayerState() || !C->GetAbilitySystemComponent()) return false;
                ++Count;
                if (It->Get()->IsLocalController()) Host=C; else if (!Remote) Remote=C;
            }
            if (Count!=Players || !Host || !Remote) return false;
            for (auto* World:Worlds)
                if (!FindPlayer(World,Host) || !FindPlayer(World,Remote)) return false;
            User=bClientLoads ? Remote:Host; Victim=bClientLoads ? Host:Remote;
            if (Players==3)
                for (FConstPlayerControllerIterator It=Server->GetPlayerControllerIterator();It;++It)
                    if (auto* C=Cast<ADeliveryCharacter>(It->Get()->GetPawn()))
                        if (C!=User.Get() && C!=Victim.Get()) Rival=C;
            if (Rival.IsValid()) for (auto* World:Worlds) if (!FindPlayer(World,Rival.Get())) return false;
            for (auto* World:Worlds)
            {
                UserCopies.Add(World,FindPlayer(World,User.Get()));
                for (TActorIterator<ADeliveryCharacter> It(World);It;++It)
                {
                    const bool IsUser=SamePlayer(*It,User.Get()),IsVictim=SamePlayer(*It,Victim.Get()),IsRival=SamePlayer(*It,Rival.Get());
                    if (!IsUser && !IsVictim && !IsRival) continue;
                    // 测试初始化重建身体，避免把旧地面和网络缓存带进场地
                    It->GetActiveRagdoll()->StopRagdoll();
                    It->SetActorLocationAndRotation(FVector(IsVictim ? -125:-225,IsRival ? 110:0,95),FRotator::ZeroRotator,false,nullptr,ETeleportType::TeleportPhysics);
                    It->GetActiveRagdoll()->StartRagdoll();
                }
            }
            auto* ASC=CastChecked<UDeliverAbilitySystemComponent>(Victim->GetAbilitySystemComponent());
            ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthRegenRateAttribute(),0.f);
            ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),0.f);
            // 正式回血 GE 使用自身周期值；本条测试手动推进门槛，先移除该测试实例的周期效果
            ASC->RemoveActiveGameplayEffectBySourceEffect(Victim->GetHealthRegenEffect(),ASC);
            FActorSpawnParameters Spawn; Spawn.Owner=User.Get(); Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            auto* Class=LoadClass<ADeliveryMotorbike>(nullptr,TEXT("/Game/Vehicle/Motorbike/BP_Motorbike.BP_Motorbike_C"));
            Bike=Server->SpawnActor<ADeliveryMotorbike>(Class,FVector(0,0,70),FRotator::ZeroRotator,Spawn);
            ++Stage; NextTime=Now+2.f; return false;
        }
        if (!Bike.IsValid() || !User.IsValid() || !Victim.IsValid())
        { Test->AddError(FString::Printf(TEXT("fixture lost stage=%d bike=%s user=%s victim=%s"),Stage,*GetNameSafe(Bike.Get()),*GetNameSafe(User.Get()),*GetNameSafe(Victim.Get()))); return End(); }
        if (Now>LastDiag+1.f)
        {
            LastDiag=Now;
            UE_LOG(LogTemp,Display,TEXT("TrunkPIE stage=%d bike=%s user=%s victim=%s mode=%d local=%d phase=%d hp=%.1f error=%.1f"),Stage,
                *Bike->GetActorLocation().ToString(),*User->GetActorLocation().ToString(),*Victim->GetActorLocation().ToString(),
                int(User->GetActiveRagdoll()->GetControlMode()),User->IsLocallyControlled(),int(Victim->TrunkPassenger->GetPhase()),
                Victim->GetAbilitySystemComponent()->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()),Victim->GetActiveRagdoll()->GetTrunkPoseError());
        }
        ADeliveryCharacter* LocalUser=nullptr;
        for (auto* World:Worlds)
            for (TActorIterator<ADeliveryCharacter> It(World);It;++It)
                if (SamePlayer(*It,User.Get()) && It->IsLocallyControlled()) LocalUser=*It;
        if (Stage<=3 && !LocalUser) return false;
        if (Stage==1)
        {
            auto* LocalVictim=FindPlayer(LocalUser->GetWorld(),Victim.Get());
            if (!LocalVictim) return false;
            const FVector Aim=LocalVictim->GetActiveRagdoll()->GetPhysicalPelvisLocation()-LocalUser->GetFollowCamera()->GetComponentLocation();
            LocalUser->GetController()->SetControlRotation(Aim.Rotation());
            User->GetController()->SetControlRotation((Victim->GetActiveRagdoll()->GetPhysicalPelvisLocation()-User->GetPawnViewLocation()).Rotation());
            if (!User->GetGrabComponent()->IsDraggingCharacter())
            {
                LocalUser->GetGrabComponent()->RequestBegin();
                NextTime=Now+.2f; return false;
            }
            if (Players==3 && !bContestChecked)
            {
                ADeliveryCharacter* LocalRival=nullptr;
                for (auto* World:Worlds)
                    if (auto* C=FindPlayer(World,Rival.Get())) if (C->IsLocallyControlled()) LocalRival=C;
                if (!LocalRival) return false;
                if (!Rival->GetGrabComponent()->IsDraggingCharacter())
                {
                    auto* RivalVictim=FindPlayer(LocalRival->GetWorld(),Victim.Get());
                    LocalRival->GetController()->SetControlRotation((RivalVictim->GetActiveRagdoll()->GetPhysicalPelvisLocation()-LocalRival->GetFollowCamera()->GetComponentLocation()).Rotation());
                    Rival->GetController()->SetControlRotation((Victim->GetActiveRagdoll()->GetPhysicalPelvisLocation()-Rival->GetPawnViewLocation()).Rotation());
                    LocalRival->GetGrabComponent()->RequestBegin();
                    NextTime=Now+.2f; return false;
                }
                Test->TestFalse(TEXT("three players cannot load contested victim"),Bike->UsePassengerTrunk(User.Get()));
                Test->TestTrue(TEXT("contested request retains original grab"),User->GetGrabComponent()->IsDraggingCharacter());
                LocalRival->GetGrabComponent()->RequestReleaseHand(true);
                LocalRival->GetGrabComponent()->RequestReleaseHand(false);
                bContestChecked=true; NextTime=Now+.5f; return false;
            }
            Test->AddInfo(TEXT("TrunkPIE real grab confirmed"));
            ++Stage; NextTime=Now+.3f; return false;
        }
        if (Stage==2)
        {
            auto* LocalBike=FindBike(LocalUser->GetWorld());
            if (!LocalBike) return false;
            auto* PC=CastChecked<ADeliveryPlayerController>(LocalUser->GetController());
            PC->ServerRequestInteraction(LocalUser,EDeliveryInteractionRequest::Use,LocalBike);
            PC->ServerRequestInteraction(LocalUser,EDeliveryInteractionRequest::Use,LocalBike);
            ++Stage; NextTime=Now+2.5f; return false;
        }
        if (Stage==3)
        {
            if (!Test->TestTrue(TEXT("E RPC loads passenger"),Victim->TrunkPassenger->GetPhase()==EDeliveryPassengerPhase::Riding)) return End();
            CheckCopies(Worlds,TEXT("loaded"),true);
            Test->TestFalse(TEXT("load releases real drag"),User->GetGrabComponent()->IsDraggingCharacter());
            Test->TestTrue(TEXT("loaded victim retains controller"),Victim->GetController()!=nullptr);
            Test->TestFalse(TEXT("loaded bike cannot summon"),Bike->SummonTo(FVector(500,0,70),0));
            Capture(Server,TEXT("Loaded"));
            if (!Test->TestTrue(TEXT("driver enters occupied bike"),Bike->TryEnter(User.Get()))) return End();
            Origin=Bike->GetActorLocation(); ++Stage; NextTime=Now+1.f; return false;
        }
        ADeliveryMotorbike* LocalBike=nullptr;
        for (auto* World:Worlds)
            for (FConstPlayerControllerIterator It=World->GetPlayerControllerIterator();It;++It)
                if (It->Get()->IsLocalController())
                    if (auto* Candidate=Cast<ADeliveryMotorbike>(It->Get()->GetPawn()))
                        if (Candidate==FindBike(World)) LocalBike=Candidate;
        if (!LocalBike) return false;
        if (Stage==4)
        {
            LocalBike->MoveInputRaw=bStress ? FVector2D(.35,.94):FVector2D(0,1);
            ++Stage; NextTime=Now+2.f; return false;
        }
        if (Stage==50)
        {
            Test->TestTrue(TEXT("opposite input brakes loaded bike"),FMath::Abs(Bike->GetCurrentSpeed())<BrakeSpeed);
            Test->TestTrue(TEXT("passenger survives braking at 10 FPS"),Victim->TrunkPassenger->GetPhase()==EDeliveryPassengerPhase::Riding);
            LocalBike->MoveInputRaw=FVector2D(0,1);
            Stage=51; NextTime=Now+1.f; return false;
        }
        if (Stage==51) { Stage=5; bBrakingChecked=true; }
        if (Stage==5)
        {
            if (bStress && !bBrakingChecked)
            {
                Test->TestTrue(TEXT("loaded bike turns on slope"),FMath::Abs(Bike->GetActorRotation().Yaw)>10.f);
                BrakeSpeed=FMath::Abs(Bike->GetCurrentSpeed());
                LocalBike->MoveInputRaw=FVector2D(0,-1);
                Stage=50; NextTime=Now+.5f; return false;
            }
            Test->TestTrue(TEXT("real driving moves loaded bike"),FVector::Dist2D(Origin,Bike->GetActorLocation())>100.f);
            CheckCopies(Worlds,TEXT("driving"),true);
            for (auto* World:Worlds) Capture(World,World==Server ? TEXT("DrivingServer"):TEXT("DrivingClient"));
            Test->TestFalse(TEXT("manual unload rejected with driver"),Bike->UsePassengerTrunk(User.Get()));
            auto* ASC=CastChecked<UDeliverAbilitySystemComponent>(Victim->GetAbilitySystemComponent());
            ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),40.f);
            Test->TestTrue(TEXT("40 percent remains stunned during exit"),ASC->IsStunned());
            Origin=Bike->GetActorLocation(); ++Stage; NextTime=Now+3.5f; return false;
        }
        LocalBike->MoveInputRaw=FVector2D::ZeroVector;
        CheckCopies(Worlds,TEXT("exited"),false);
        Test->TestTrue(TEXT("bike continues during automatic exit"),FVector::Dist2D(Origin,Bike->GetActorLocation())>100.f);
        Test->TestFalse(TEXT("wake after exit"),CastChecked<UDeliverAbilitySystemComponent>(Victim->GetAbilitySystemComponent())->IsStunned());
        Test->TestTrue(TEXT("passenger motors removed"),Victim->GetActiveRagdoll()->TrunkControls.IsEmpty());
        return End();
    }
private:
    static bool SamePlayer(const ADeliveryCharacter* A,const ADeliveryCharacter* B)
    { return A && B && A->GetPlayerState() && B->GetPlayerState() && A->GetPlayerState()->GetPlayerId()==B->GetPlayerState()->GetPlayerId(); }
    static ADeliveryCharacter* FindPlayer(UWorld* World,ADeliveryCharacter* Player)
    { for (TActorIterator<ADeliveryCharacter> It(World);It;++It) if (SamePlayer(*It,Player)) return *It; return nullptr; }
    ADeliveryMotorbike* FindBike(UWorld* World)
    {
        if (World==Bike->GetWorld()) return Bike.Get();
        for (TActorIterator<ADeliveryMotorbike> It(World);It;++It)
            if (UserCopies.Contains(World) && (It->GetOwner()==UserCopies[World].Get() || It->GetDriver()==UserCopies[World].Get())) return *It;
        return nullptr;
    }
    void Capture(UWorld* World,const TCHAR* Label)
    {
        auto* Camera=World->SpawnActor<ASceneCapture2D>();
        auto* Capture=Camera->GetCaptureComponent2D();
        auto* Target=NewObject<UTextureRenderTarget2D>(Camera);
        Target->RenderTargetFormat=RTF_RGBA8; Target->InitAutoFormat(1024,768);
        Capture->TextureTarget=Target; Capture->CaptureSource=SCS_FinalColorLDR;
        auto* ViewBike=FindBike(World);
        const FVector Focus=ViewBike->GetPassengerFrame().GetLocation()+FVector(0,0,20);
        const FVector Position=Focus+FVector(-230,-270,140);
        Camera->SetActorLocationAndRotation(Position,(Focus-Position).Rotation());
        Capture->CaptureScene();
        const FString Name=FString::Printf(TEXT("Trunk_%s_%d_%s.png"),bClientLoads ? TEXT("Client"):TEXT("Host"),Players,Label);
        UKismetRenderingLibrary::ExportRenderTarget(World,Target,FPaths::ProjectSavedDir()/TEXT("Automation/TrunkScreenshots"),Name);
        Camera->Destroy();
    }
    void CheckCopies(const TArray<UWorld*>& Worlds,const TCHAR* Label,bool Occupied)
    {
        for (auto* World:Worlds)
        {
            auto* Copy=FindPlayer(World,Victim.Get());
            auto* CopyBike=FindBike(World);
            if (!Test->TestNotNull(TEXT("passenger copy"),Copy) || !Test->TestNotNull(TEXT("bike copy"),CopyBike)) continue;
            Test->TestEqual(TEXT("replicated occupancy"),CopyBike->HasTrunkPassenger(),Occupied);
            Test->TestEqual(TEXT("replicated passenger state"),Copy->TrunkPassenger->IsPassenger(),Occupied);
            Test->TestTrue(TEXT("all copies retain physics"),Copy->GetMesh()->IsSimulatingPhysics(TEXT("Hips")));
            if (Occupied)
            {
                const float Error=FVector::Dist(Copy->GetActiveRagdoll()->GetPhysicalPelvisLocation(),Victim->GetActiveRagdoll()->GetPhysicalPelvisLocation());
                const float RelativeError=FVector::Dist(CopyBike->GetPassengerFrame().InverseTransformPosition(Copy->GetActiveRagdoll()->GetPhysicalPelvisLocation()),
                    Bike->GetPassengerFrame().InverseTransformPosition(Victim->GetActiveRagdoll()->GetPhysicalPelvisLocation()));
                Test->AddInfo(FString::Printf(TEXT("TrunkPIE %s net=%d error=%.1f relativeError=%.1f bike=%s serverBike=%s hips=%s"),Label,int(World->GetNetMode()),Error,RelativeError,
                    *CopyBike->GetActorLocation().ToString(),*Bike->GetActorLocation().ToString(),*Copy->GetActiveRagdoll()->GetPhysicalPelvisLocation().ToString()));
                Test->TestTrue(TEXT("remote physical pelvis follows server"),Error<90.f);
                Test->TestTrue(TEXT("passenger remains aligned with local bike"),RelativeError<60.f);
            }
        }
    }
    bool End()
    {
        if (bStress) if (auto* FPS=IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"))) FPS->Set(PreviousFPS,ECVF_SetByCode);
        GEditor->RequestEndPlayMap(); bEnding=true; return false;
    }
    FAutomationTestBase* Test;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TWeakObjectPtr<ADeliveryCharacter> User,Victim,Rival;
    TWeakObjectPtr<ADeliveryMotorbike> Bike;
    TMap<UWorld*,TWeakObjectPtr<ADeliveryCharacter>> UserCopies;
    FVector Origin=FVector::ZeroVector;
    double Started=0;
    float NextTime=0,LastDiag=0,PreviousFPS=0,BrakeSpeed=0;
    int Stage=0;
    bool bRequested=false,bEnding=false,bClientLoads,bContestChecked=false,bStress=false,bBrakingChecked=false;
    int Players;
};

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDeliveryTrunkPassengerPIETest,"Delivery.Vehicle.TrunkMultiplayer",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FDeliveryTrunkPassengerPIETest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
    Names={TEXT("ClientLoads"),TEXT("HostLoads"),TEXT("ThreePlayers"),TEXT("SlopeTurnBrakeLowFPS")}; Commands=Names;
}
bool FDeliveryTrunkPassengerPIETest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryTrunkPassengerPIECommand(this,Parameters!=TEXT("HostLoads"),Parameters==TEXT("ThreePlayers") ? 3:2,Parameters==TEXT("SlopeTurnBrakeLowFPS")));
    return true;
}
#endif
