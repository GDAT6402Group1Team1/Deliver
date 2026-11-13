#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcConversationAimTest,"Delivery.AI.ConversationAim",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcConversationAimTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bWasEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        Settings->bEnabled=bWasEnabled;
    };

    // 使用实际控制器、交谈组件和碰撞查询，只由测试提供镜头位置与方向
    const auto CreatePlayer=[&](FVector Location,APlayerController*& Controller,APawn*& Pawn)
    {
        Pawn=World->SpawnActor<APawn>();
        auto* Root=NewObject<USceneComponent>(Pawn);
        Pawn->SetRootComponent(Root);
        Root->RegisterComponent();
        Pawn->SetActorLocation(Location);
        Controller=World->SpawnActor<APlayerController>();
        Controller->Possess(Pawn);
        auto* Conversation=NewObject<UDeliveryConversationComponent>(Controller);
        Conversation->RegisterComponent();
        return Conversation;
    };
    APlayerController* FirstController=nullptr;
    APawn* FirstPlayer=nullptr;
    auto* Conversation=CreatePlayer(FVector(0,0,90),FirstController,FirstPlayer);
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* FarNpc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(450,0,90),FRotator::ZeroRotator,Spawn);
    auto* NearNpc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(180,160,90),FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("创建远处 NPC"),FarNpc) || !TestNotNull(TEXT("创建近处 NPC"),NearNpc)) return false;
    FarNpc->SetActorTickEnabled(false);
    NearNpc->SetActorTickEnabled(false);
    const FVector View(-150,0,130);
    const auto AimAt=[&](ADeliveryNpcCharacter* Npc)
    {
        return Conversation->FindNpcInView(View,(Npc->GetActorLocation()-View).GetSafeNormal());
    };
    TestTrue(TEXT("远近两人原本都允许交谈"),Conversation->CanTalkTo(FarNpc) && Conversation->CanTalkTo(NearNpc));
    TestTrue(TEXT("瞄准较远的人时，不会选中近处旁人"),AimAt(FarNpc)==FarNpc);
    TestTrue(TEXT("转动镜头后改为选择新对准的人"),AimAt(NearNpc)==NearNpc);
    TestTrue(TEXT("转回镜头后立即恢复原目标"),AimAt(FarNpc)==FarNpc);
    TestNull(TEXT("镜头朝后时不选择身后两个 NPC"),Conversation->FindNpcInView(View,-FVector::ForwardVector));
    TestNull(TEXT("看向两人之间的空处时不回退到最近的人"),
        Conversation->FindNpcInView(View,FVector(1,-1,0).GetSafeNormal()));
    TestNull(TEXT("无效朝向不会选中人物"),Conversation->FindNpcInView(View,FVector::ZeroVector));

    NearNpc->SetActorLocation(FVector(200,0,90));
    TestTrue(TEXT("前后两人重叠在准星中时选择前面的人"),AimAt(FarNpc)==NearNpc);
    NearNpc->SetActorLocation(FVector(200,41,90));
    TestTrue(TEXT("真正对准身体的人优先于擦过准星边缘的人"),
        Conversation->FindNpcInView(FVector(0,0,90),FVector::ForwardVector)==FarNpc);
    NearNpc->SetActorLocation(FVector(2000,1500,90));
    TestTrue(TEXT("对准头部也能交谈，不要求准星对着角色中心"),
        Conversation->FindNpcInView(View,(FarNpc->GetActorLocation()+FVector(0,0,75)-View).GetSafeNormal())==FarNpc);

    auto* Wall=World->SpawnActor<AActor>();
    auto* WallBox=NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(WallBox);
    WallBox->SetBoxExtent(FVector(20,60,120));
    WallBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    WallBox->SetCollisionObjectType(ECC_WorldStatic);
    WallBox->SetCollisionResponseToAllChannels(ECR_Ignore);
    WallBox->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
    WallBox->RegisterComponent();
    Wall->SetActorLocation(FVector(200,0,110));
    TestFalse(TEXT("玩家与 NPC 之间有墙时拒绝交谈"),Conversation->CanTalkTo(FarNpc));
    TestNull(TEXT("准星隔着墙时不会选中 NPC"),AimAt(FarNpc));
    Wall->SetActorLocation(FVector(-80,0,110));
    TestTrue(TEXT("只挡住身后镜头的墙不影响玩家到 NPC 的检查"),Conversation->CanTalkTo(FarNpc));
    TestNull(TEXT("玩家能靠近交谈，但镜头被墙挡住时仍不选中"),AimAt(FarNpc));
    WallBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TestTrue(TEXT("移开遮挡后可以再次选中"),AimAt(FarNpc)==FarNpc);

    FarNpc->SetActorLocation(FVector(599,0,90));
    TestTrue(TEXT("600 厘米以内可以选中"),AimAt(FarNpc)==FarNpc);
    FarNpc->SetActorLocation(FVector(601,0,90));
    TestFalse(TEXT("超过 600 厘米时交谈规则拒绝"),Conversation->CanTalkTo(FarNpc));
    TestNull(TEXT("超过距离后即使仍在准星中也不选中"),AimAt(FarNpc));
    FarNpc->SetActorLocation(FVector(400,0,90));
    TestTrue(TEXT("使目标进入倒地状态"),FarNpc->Combat->SetDeliberateProne(true));
    TestFalse(TEXT("倒地者不能交谈"),Conversation->CanTalkTo(FarNpc));
    TestNull(TEXT("不会选中对准的倒地者"),AimAt(FarNpc));
    TestTrue(TEXT("恢复目标的站立状态"),FarNpc->Combat->SetDeliberateProne(false));
    TestTrue(TEXT("恢复后重新允许选中"),AimAt(FarNpc)==FarNpc);

    // 两个组件分别使用自己的玩家位置，交替查询不会留下共享目标
    APlayerController* SecondController=nullptr;
    APawn* SecondPlayer=nullptr;
    auto* SecondConversation=CreatePlayer(FVector(1000,0,90),SecondController,SecondPlayer);
    NearNpc->SetActorLocation(FVector(1400,0,90));
    const FVector SecondView(850,0,130);
    TestTrue(TEXT("第二位玩家选择自己面前的人"),SecondConversation->FindNpcInView(
        SecondView,(NearNpc->GetActorLocation()-SecondView).GetSafeNormal())==NearNpc);
    TestTrue(TEXT("第二位玩家查询后，第一位玩家仍选择原目标"),AimAt(FarNpc)==FarNpc);
    TestFalse(TEXT("第一位玩家不能借用第二位玩家的交谈距离"),Conversation->CanTalkTo(NearNpc));
    SecondController->UnPossess();
    TestNull(TEXT("失去控制角色的控制器不能继续选择 NPC"),
        SecondConversation->FindNpcInView(SecondView,FVector::ForwardVector));
    TestTrue(TEXT("一位玩家失去角色不影响另一位玩家"),AimAt(FarNpc)==FarNpc);
    return true;
}
#endif
