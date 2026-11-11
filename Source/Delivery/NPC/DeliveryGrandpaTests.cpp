#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "NPC/DeliveryGrandpa.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Character/DeliveryCharacter.h"
#include "Camera/DeliveryDoorHitCameraShake.h"
#include "Camera/CameraModifier_CameraShake.h"
#include "Camera/PlayerCameraManager.h"
#include "Economy/DeliveryWalletComponent.h"
#include "Inventory/DeliveryCaterpillarItem.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "Carry/DeliveryCarryComponent.h"
#include "Interaction/DeliveryInteractionComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Player/DeliveryPlayerController.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/World/DeliveryBreakableDoor.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryGrandpaAssetsTest, "Delivery.NPC.Grandpa.Assets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryGrandpaAssetsTest::RunTest(const FString&)
{
    UClass* Class = LoadClass<ADeliveryGrandpa>(nullptr, TEXT("/Game/NPC/Grandpa/BP_Grandpa.BP_Grandpa_C"));
    if (!TestNotNull(TEXT("爷爷蓝图已保存"), Class)) return false;
    const auto* CDO = Class->GetDefaultObject<ADeliveryGrandpa>();
    TestNotNull(TEXT("使用提供的模型"), CDO->Body->GetSkeletalMeshAsset());
    TestNotNull(TEXT("行为树"), CDO->Behavior.Get());
    if (CDO->Behavior)
    {
        TestNotNull(TEXT("可编辑行为树图"), CDO->Behavior->BTGraph.Get());
        TestNotNull(TEXT("运行根节点"), CDO->Behavior->RootNode.Get());
        TestNotNull(TEXT("黑板"), CDO->Behavior->BlackboardAsset.Get());
        if (CDO->Behavior->RootNode)
        {
            TestEqual(TEXT("四个优先级分支"), CDO->Behavior->RootNode->Children.Num(), 4);
            for (int Index = 0; Index < 3 && Index < CDO->Behavior->RootNode->Children.Num(); ++Index)
            {
                const auto& Branch = CDO->Behavior->RootNode->Children[Index];
                TestEqual(TEXT("条件装饰器已序列化"), Branch.Decorators.Num(), 1);
                if (Branch.Decorators.Num()) TestNotNull(TEXT("装饰器有效"), Branch.Decorators[0].Get());
            }
        }
    }
    for (UAnimSequence* Clip : {CDO->IdleAnimation.Get(), CDO->TalkAnimation.Get(), CDO->StartledAnimation.Get(), CDO->ReceiveAnimation.Get(), CDO->WalkAnimation.Get()})
    {
        if (!TestNotNull(TEXT("动画存在"), Clip)) continue;
        TestTrue(TEXT("动画有有效时长"), Clip->GetPlayLength() > .2f);
        TestTrue(TEXT("动画匹配爷爷骨架"), Clip->GetSkeleton() == CDO->Body->GetSkeletalMeshAsset()->GetSkeleton());
        TestFalse(TEXT("不应用根运动"), Clip->bEnableRootMotion);
        FTransform RootPose;
        Clip->GetBoneTransform(RootPose, FSkeletonPoseBoneIndex(0), FAnimExtractContext(0.0), true);
        const auto& Reference = Clip->GetSkeleton()->GetReferenceSkeleton().GetRefBonePose()[0];
        AddInfo(FString::Printf(TEXT("%s root=%s reference=%s"), *Clip->GetName(), *RootPose.ToString(), *Reference.ToString()));
        TestTrue(TEXT("动画根轴与静止骨架一致"), RootPose.GetRotation().AngularDistance(Reference.GetRotation()) < .01f);
    }
    TestTrue(TEXT("绑定第一单"), CDO->DeliveryTarget->ExpectedTask && CDO->DeliveryTarget->ExpectedTask->TaskId == TEXT("Task_001"));
    return true;
}

// 在原地图开启真实监听服务器和远端客户端，使用玩家所属 RPC 验证长按、复制及交付
// 固定测试角色位置以排除物理手感波动；远端持斧输入、服务器扫掠和交付串成完整流程
class FDeliveryGrandpaPIECommand : public IAutomationLatentCommand
{
public:
    explicit FDeliveryGrandpaPIECommand(FAutomationTestBase* InTest, bool InCaterpillar = false) : Test(InTest), bTestCaterpillar(InCaterpillar) {}
    bool Update() override
    {
        UWorld* Server = nullptr;
        UWorld* Client = nullptr;
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
            FRequestPlaySessionParams Params;
            Params.EditorPlaySettings = Settings.Get(); Params.bAllowOnlineSubsystem = false;
            GEditor->RequestPlaySession(Params);
            bRequested = true; Started = FPlatformTime::Seconds(); return false;
        }
        if (FPlatformTime::Seconds()-Started > 240) { Test->AddError(FString::Printf(TEXT("爷爷双人测试超时 stage=%d"),Stage)); return End(); }
        if (!Server || !Client) return false;
        auto* Tasks = UDeliveryTaskManagerComponent::Get(Server);
        ADeliveryGrandpa* Grandpa = nullptr; ADeliveryGrandpa* RemoteGrandpa = nullptr;
        for (TActorIterator<ADeliveryGrandpa> It(Server); It; ++It) Grandpa = *It;
        for (TActorIterator<ADeliveryGrandpa> It(Client); It; ++It) RemoteGrandpa = *It;
        auto* PC = Cast<ADeliveryPlayerController>(Client->GetFirstPlayerController());
        auto* Character = PC ? Cast<ADeliveryCharacter>(PC->GetPawn()) : nullptr;
        ADeliveryCharacter* AuthorityCharacter = nullptr;
        ADeliveryCharacter* HostCharacter = nullptr;
        for (FConstPlayerControllerIterator It = Server->GetPlayerControllerIterator(); It; ++It)
            if (auto* Player = Cast<ADeliveryPlayerController>(It->Get()))
            {
                if (Player->IsLocalController()) HostCharacter = Cast<ADeliveryCharacter>(Player->GetPawn());
                else AuthorityCharacter = Cast<ADeliveryCharacter>(Player->GetPawn());
            }
        if (!Tasks || !Grandpa || !RemoteGrandpa || !Character || !AuthorityCharacter || !HostCharacter || !Grandpa->FrontDoor || !RemoteGrandpa->FrontDoor) return false;
        auto* Task = Grandpa->DeliveryTarget->ExpectedTask.Get();
        const float Now = Server->GetTimeSeconds();
        auto HasDoorShake = [](ADeliveryCharacter* Pawn)
        {
            const auto* Controller = Cast<APlayerController>(Pawn->GetController());
            auto* Manager = Controller ? Controller->PlayerCameraManager.Get() : nullptr;
            auto* Modifier = Manager ? Cast<UCameraModifier_CameraShake>(Manager->FindCameraModifierByClass(UCameraModifier_CameraShake::StaticClass())) : nullptr;
            TArray<FActiveCameraShakeInfo> Shakes;
            if (Modifier) Modifier->GetActiveCameraShakes(Shakes);
            return Shakes.ContainsByPredicate([](const FActiveCameraShakeInfo& Info) { return Info.ShakeInstance && Info.ShakeInstance->IsA<UDeliveryDoorHitCameraShake>(); });
        };
        bClientSawShake |= HasDoorShake(Character);
        bHostSawShake |= HasDoorShake(HostCharacter);
        if (bTrackMovement)
            MaxStrollDistance = FMath::Max(MaxStrollDistance, FVector::Dist2D(Grandpa->GetActorLocation(), StrollOrigin));
        if (Now < NextTime) return false;
        auto Send = [&](EDeliveryInteractionRequest Request, AActor* Target)
        {
            AActor* ServerTarget = Target == RemoteGrandpa ? static_cast<AActor*>(Grandpa) : Grandpa->FrontDoor.Get();
            const auto* Action = ServerTarget->FindComponentByClass<UDeliveryInteractableComponent>();
            UE_LOG(LogTemp, Display, TEXT("GrandpaPIE request=%d canUse=%d mode=%d distance=%.1f pos=%s package=%s"),
                int(Request), AuthorityCharacter->GetInteractionComponent()->CanUse(ServerTarget),
                int(AuthorityCharacter->GetActiveRagdoll()->GetControlMode()), Action ? Action->GetInteractionDistance(AuthorityCharacter) : -1.f,
                *AuthorityCharacter->GetActorLocation().ToString(), Package.IsValid() ? *Package->GetActorLocation().ToString() : TEXT("none"));
            if (Action) UE_LOG(LogTemp, Display, TEXT("GrandpaPIE target point=%s"), *Action->GetInteractionPoint(AuthorityCharacter->GetActorLocation()).ToString());
            PC->ServerRequestInteraction(Character, Request, Target);
        };
        auto Move = [&](ADeliveryCharacter* Pawn, FVector Position)
        {
            Pawn->SetActorTickEnabled(false);
            // 布娃娃另有 PostPhysics Tick，单关 Primary Tick 仍会把胶囊拉回旧髋位置
            Pawn->GetActiveRagdoll()->RegisterAllComponentTickFunctions(false);
            Pawn->GetMesh()->SetSimulatePhysics(false);
            Pawn->GetMesh()->SetAllBodiesSimulatePhysics(false);
            Pawn->GetMesh()->AttachToComponent(Pawn->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
            Pawn->SetActorLocation(Position, false, nullptr, ETeleportType::TeleportPhysics);
        };
        auto Step = [&](float Delay = .8f) { UE_LOG(LogTemp, Display, TEXT("GrandpaPIE stage %d complete"), Stage); ++Stage; NextTime = Now + Delay; };
        switch (Stage)
        {
        case 0:
            InitialClientBalance = UDeliveryWalletComponent::FindWallet(AuthorityCharacter)->GetBalance();
            InitialHostBalance = UDeliveryWalletComponent::FindWallet(HostCharacter)->GetBalance();
            StrollOrigin = Grandpa->GetActorLocation(); bTrackMovement = true;
            if (Tasks->GetTaskStatus(Task) != EDeliveryTaskStatus::Offered) return false;
            Test->TestTrue(TEXT("爷爷竖直站立"), FVector::DotProduct(Grandpa->GetActorUpVector(), FVector::UpVector) > .99f);
            Test->TestTrue(TEXT("大门竖直堵住门洞"), FVector::DotProduct(Grandpa->FrontDoor->GetActorUpVector(), FVector::UpVector) > .99f);
            Test->TestTrue(TEXT("接听第一单"), Tasks->StartTaskTimerFromAcceptedCall(Task));
            if (bTestCaterpillar)
            {
                UClass* FoodClass = LoadClass<ADeliveryPackageItem>(nullptr, TEXT("/Game/Blueprint/Item/BP_DeliveryPackage.BP_DeliveryPackage_C"));
                if (!Test->TestNotNull(TEXT("正式外卖蓝图存在"), FoodClass)) return End();
                Package = Server->SpawnActor<ADeliveryPackageItem>(FoodClass);
                Package->GetDeliveryItemComponent()->OwningTask = Task;
            }
            else Package = Server->SpawnActor<ADeliveryTestPackage>();
            Test->TestTrue(TEXT("远端玩家取得正确包裹"), AuthorityCharacter->GetCarryComponent()->TryReceivePackage(Package.Get(), false));
            Test->TestFalse(TEXT("完整门阻止交付"), Grandpa->DeliveryTarget->TryDeliver(Package.Get(), AuthorityCharacter->GetTaskPlayerState()));
            Move(HostCharacter, FVector(10100,13350,2080));
            Move(AuthorityCharacter, FVector(9812,13380,2080)); Move(Character, FVector(9812,13380,2080));
            Test->TestNotNull(TEXT("服务器运行 AIController"), Grandpa->GetController());
            Test->TestNull(TEXT("客户端不重复运行 AI"), RemoteGrandpa->GetController());
            Step(); break;
        case 1:
            Send(EDeliveryInteractionRequest::CompleteHold, RemoteGrandpa->FrontDoor);
            Step(); break;
        case 2:
            Test->TestEqual(TEXT("伪造完成长按无效"), Grandpa->GetKnockCount(), 0);
            Send(EDeliveryInteractionRequest::BeginHold, RemoteGrandpa->FrontDoor);
            Step(); break;
        case 3:
            Send(EDeliveryInteractionRequest::CompleteHold, RemoteGrandpa->FrontDoor);
            Step(); break;
        case 4:
            Test->TestEqual(TEXT("第一次敲门记录一次"), Grandpa->GetKnockCount(), 1);
            Test->TestEqual(TEXT("次数复制到客户端"), RemoteGrandpa->GetKnockCount(), 1);
            Test->TestTrue(TEXT("第一次无回应，继续屋内活动"), Grandpa->GetCurrentAction() >= EDeliveryGrandpaAction::StrollOut);
            Send(EDeliveryInteractionRequest::CompleteHold, RemoteGrandpa->FrontDoor);
            Send(EDeliveryInteractionRequest::BeginHold, RemoteGrandpa->FrontDoor);
            Step(); break;
        case 5:
            Send(EDeliveryInteractionRequest::CompleteHold, RemoteGrandpa->FrontDoor);
            Step(.4f); break;
        case 6:
            Test->TestEqual(TEXT("第二次回应"), Grandpa->GetKnockCount(), 2);
            Test->TestEqual(TEXT("蓝图 BTTask 选择回复动作"), Grandpa->GetCurrentAction(), EDeliveryGrandpaAction::Reply);
            Test->TestEqual(TEXT("回复动作复制"), RemoteGrandpa->GetCurrentAction(), EDeliveryGrandpaAction::Reply);
            Step(12.f); break;
        case 7:
            if (!Axe.IsValid())
            {
                Test->TestTrue(TEXT("爷爷确实在屋内走动"), MaxStrollDistance > 25.f);
                bTrackMovement = false;
                Test->TestFalse(TEXT("持续等待不会开门"), Grandpa->IsDoorBroken());
                Test->TestTrue(TEXT("回应结束回到屋内活动"), Grandpa->GetCurrentAction() >= EDeliveryGrandpaAction::StrollOut);
                Test->TestFalse(TEXT("等待后仍不能交付"), Grandpa->DeliveryTarget->TryDeliver(Package.Get(), AuthorityCharacter->GetTaskPlayerState()));
                for (TActorIterator<ADeliveryHandheldItem> It(Server); It; ++It)
                    if (It->GetClass()->GetName() == TEXT("BP_Axe_C")) { Axe = *It; break; }
                if (!Test->TestTrue(TEXT("关卡有正式斧头"), Axe.IsValid())) return End();
                if (!Test->TestTrue(TEXT("斧头拾入背包"), AuthorityCharacter->GetInventoryComponent()->TryPickup(Axe.Get()))) return End();
                NextTime = Now + .8f; return false;
            }
            if (!bSelectedAxe)
            {
                Character->GetInventoryComponent()->RequestSlotAction(1);
                bSelectedAxe = true; NextTime = Now + .8f; return false;
            }
            if (!Test->TestTrue(TEXT("远端切槽后服务器持斧"), AuthorityCharacter->GetInventoryComponent()->GetHeldItem() == Axe.Get())) return End();
            Test->TestEqual(TEXT("每次远端挥砍只命中一次"), Grandpa->FrontDoor->GetHitCount(), AxeSwings);
            Test->TestEqual(TEXT("斧击次数复制到远端"), RemoteGrandpa->FrontDoor->GetHitCount(), AxeSwings);
            if (AxeSwings < 3)
            {
                if (!Character->GetActiveRagdoll()->CanStartAxeSwing()) { NextTime = Now + .2f; return false; }
                Test->TestFalse(TEXT("前三击完成前门保持完整"), Grandpa->IsDoorBroken());
                for (ADeliveryCharacter* Pawn : {AuthorityCharacter, Character})
                {
                    Pawn->GetActiveRagdoll()->CurrentFacingYaw = 90.f;
                    Pawn->SetActorRotation(FRotator(0,90,0));
                    Pawn->GetMesh()->RefreshBoneTransforms();
                }
                // 快递已按正常切槽规则落地，放在门外避免测试期间滚入其他碰撞体
                Package->GetItemMesh()->SetSimulatePhysics(false);
                Package->SetActorLocation(FVector(9700,13250,2050));
                if (AxeSwings == 2) StartledOrigin = Grandpa->GetActorLocation();
                Character->DoAttackLeft();
                ++AxeSwings;
                NextTime = Now + .85f; return false;
            }
            Test->TestTrue(TEXT("第三次真实挥斧扫掠破门"), Grandpa->IsDoorBroken());
            Test->TestFalse(TEXT("重复破门被拒绝"), Grandpa->FrontDoor->BreakFromAxe());
            Step(.1f); break;
        case 8:
            Test->TestTrue(TEXT("破门复制给远端"), RemoteGrandpa->IsDoorBroken());
            Test->TestEqual(TEXT("蓝图选择受惊"), Grandpa->GetCurrentAction(), EDeliveryGrandpaAction::Startled);
            Test->TestEqual(TEXT("受惊动作复制到远端"), RemoteGrandpa->GetCurrentAction(), EDeliveryGrandpaAction::Startled);
            Test->TestFalse(TEXT("远距离不能交付"), Grandpa->DeliveryTarget->TryDeliver(Package.Get(), AuthorityCharacter->GetTaskPlayerState()));
            Move(AuthorityCharacter, Grandpa->GetActorLocation() + FVector(0,-100,12)); Move(Character, Grandpa->GetActorLocation() + FVector(0,-100,12));
            Step(2.8f); break;
        case 9:
            if (bTestCaterpillar && !bCaterpillarComplete) { Stage = 20; return false; }
            if (!bReacquiredPackage)
            {
                Test->TestTrue(TEXT("挥斧客户端收到受击镜头反馈"), bClientSawShake);
                Test->TestFalse(TEXT("旁观服务器玩家不晃镜头"), bHostSawShake);
                Test->TestEqual(TEXT("受惊后等待收件"), Grandpa->GetCurrentAction(), EDeliveryGrandpaAction::Receive);
                Test->TestTrue(TEXT("受惊确实向屋内后退"), FVector::Dist2D(StartledOrigin, Grandpa->GetActorLocation()) > 15.f);
                if (AuthorityCharacter->GetCarriedTaskPackage() != Package.Get() &&
                    !Test->TestTrue(TEXT("破门后重新拿起外卖，斧头自动收进背包"), AuthorityCharacter->GetCarryComponent()->TryReceivePackage(Package.Get(), false))) return End();
                // 爷爷后退结束后再靠近，等待手持表现更新，不能只检查角色中心的距离
                Move(AuthorityCharacter, Grandpa->GetActorLocation() + FVector(0,-75,12));
                Move(Character, Grandpa->GetActorLocation() + FVector(0,-75,12));
                bReacquiredPackage = true;
                NextTime = Now + .8f; return false;
            }
            Test->TestFalse(TEXT("其他玩家不能代交包裹"), Grandpa->DeliveryTarget->TryDeliver(Package.Get(), HostCharacter->GetTaskPlayerState()));
            if (!Test->TestTrue(TEXT("靠近后持有者满足交付条件"), AuthorityCharacter->GetInteractionComponent()->CanUse(Grandpa))) return End();
            Send(EDeliveryInteractionRequest::BeginHold, RemoteGrandpa);
            Step(); break;
        case 10:
            Send(EDeliveryInteractionRequest::CompleteHold, RemoteGrandpa);
            Step(); break;
        case 11:
            if (!Test->TestEqual(TEXT("客户端交付推进共享任务"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::Completed)) return End();
            Test->TestTrue(TEXT("爷爷记录已收件"), Grandpa->HasReceivedDelivery());
            Test->TestTrue(TEXT("收件复制"), RemoteGrandpa->HasReceivedDelivery());
            Test->TestNull(TEXT("交付后清空携带"), AuthorityCharacter->GetCarriedTaskPackage());
            Test->TestFalse(TEXT("包裹已销毁"), Package.IsValid());
            PaidBalance = UDeliveryWalletComponent::FindWallet(AuthorityCharacter)->GetBalance();
            if (bTestCaterpillar)
            {
                Test->TestEqual(TEXT("交付计入一次毛毛虫倍率"), Tasks->PreviewReward(Task).SpecialMultiplier, 1.25f);
                Test->TestEqual(TEXT("奖励明细与实际钱包一致"), PaidBalance - InitialClientBalance, Tasks->PreviewReward(Task).FinalReward);
            }
            Test->TestTrue(TEXT("奖励已入账交付者"), PaidBalance > InitialClientBalance);
            Test->TestEqual(TEXT("旁观者未获奖励"), UDeliveryWalletComponent::FindWallet(HostCharacter)->GetBalance(), InitialHostBalance);
            Test->TestFalse(TEXT("重复提交不能二次结算"), Tasks->TryCompleteDelivery(Task, HostCharacter->GetTaskPlayerState()));
            Test->TestEqual(TEXT("蓝图播放收件感谢"), Grandpa->GetCurrentAction(), EDeliveryGrandpaAction::Thanks);
            Step(3.f); break;
        case 20:
            // 准备可交互站位，后续拾取、投放和重新取件均经远端玩家 RPC
            Move(AuthorityCharacter, Grandpa->GetActorLocation() + FVector(0,-75,12));
            Move(Character, Grandpa->GetActorLocation() + FVector(0,-75,12));
            Character->GetInventoryComponent()->RequestSlotAction(1);
            Package->SetActorLocation(AuthorityCharacter->GetActorLocation() + FVector(0,35,-20));
            {
                UClass* WormClass = LoadClass<ADeliveryCaterpillarItem>(nullptr, TEXT("/Game/Blueprint/Item/BP_Caterpillar.BP_Caterpillar_C"));
                if (!Test->TestNotNull(TEXT("正式毛毛虫蓝图存在"), WormClass)) return End();
                Worm = Server->SpawnActor<ADeliveryCaterpillarItem>(WormClass);
                Worm->GetItemMesh()->SetSimulatePhysics(false);
                Worm->SetActorLocation(AuthorityCharacter->GetActorLocation() + FVector(25,0,-10));
            }
            Step(); break;
        case 21:
            for (TActorIterator<ADeliveryCaterpillarItem> It(Client); It; ++It)
                if (It->GetFName() == Worm->GetFName()) RemoteWorm = *It;
            for (TActorIterator<ADeliveryPackageItem> It(Client); It; ++It)
                if (It->GetFName() == Package->GetFName()) RemotePackage = *It;
            if (!Test->TestTrue(TEXT("毛毛虫和外卖复制到远端"), RemoteWorm.IsValid() && RemotePackage.IsValid())) return End();
            PC->ServerRequestInteraction(Character, EDeliveryInteractionRequest::Use, RemoteWorm.Get());
            Step(); break;
        case 22:
            if (!Test->TestTrue(TEXT("远端 E 拾取毛毛虫"), AuthorityCharacter->GetInventoryComponent()->GetHeldItem() == Worm.Get())) return End();
            Test->TestFalse(TEXT("另一玩家不能重复拿走已持有毛毛虫"), HostCharacter->GetInventoryComponent()->TryPickup(Worm.Get()));
            Test->TestTrue(TEXT("服务器允许向近处外卖投放"), Package->CanInsertCaterpillar(AuthorityCharacter));
            {
                const FVector Position = Package->GetActorLocation();
                Package->SetActorLocation(Position + FVector(500,0,0));
                Test->TestFalse(TEXT("远距离投放被拒绝"), Package->CanInsertCaterpillar(AuthorityCharacter));
                Package->SetActorLocation(Position);
                Package->SetOwner(HostCharacter);
                Test->TestFalse(TEXT("别人持有的外卖不能投放"), Package->CanInsertCaterpillar(AuthorityCharacter));
                Package->SetOwner(nullptr);
                Package->GetDeliveryItemComponent()->OwningTask = nullptr;
                Test->TestFalse(TEXT("非任务外卖不能投放"), Package->CanInsertCaterpillar(AuthorityCharacter));
                Package->GetDeliveryItemComponent()->OwningTask = Task;
                AActor* Wall = Server->SpawnActor<AActor>();
                auto* Box = NewObject<UBoxComponent>(Wall);
                Wall->SetRootComponent(Box);
                Box->SetBoxExtent(FVector(50,3,60));
                Box->SetCollisionProfileName(TEXT("BlockAll"));
                Box->RegisterComponent();
                Wall->SetActorLocation((Position + AuthorityCharacter->GetActorLocation()) * .5f);
                Test->TestFalse(TEXT("隔墙投放被拒绝"), Package->CanInsertCaterpillar(AuthorityCharacter));
                Wall->Destroy();
            }
            PC->ServerRequestInteraction(Character, EDeliveryInteractionRequest::CompleteHold, RemotePackage.Get());
            Step(); break;
        case 23:
            Test->TestFalse(TEXT("伪造投放完成不生效"), Package->ContainsCaterpillar());
            PC->ServerRequestInteraction(Character, EDeliveryInteractionRequest::BeginHold, RemotePackage.Get());
            Step(); break;
        case 24:
            PC->ServerRequestInteraction(Character, EDeliveryInteractionRequest::CompleteHold, RemotePackage.Get());
            Step(); break;
        case 25:
            if (!Test->TestTrue(TEXT("服务器外卖已加入毛毛虫"), Package->ContainsCaterpillar())) return End();
            Test->TestTrue(TEXT("外卖状态复制到客户端"), RemotePackage->ContainsCaterpillar());
            for (ADeliveryPackageItem* Food : {Package.Get(), RemotePackage.Get()})
                for (UStaticMeshComponent* Visual : TInlineComponentArray<UStaticMeshComponent*>(Food))
                    if (Visual->GetFName() == TEXT("CaterpillarVisual"))
                    {
                        Test->TestTrue(TEXT("正式外卖有毛毛虫显示模型"), Visual->GetStaticMesh() != nullptr);
                        Test->TestFalse(TEXT("两端都显示已加入的毛毛虫"), Visual->bHiddenInGame);
                    }
            Test->TestFalse(TEXT("消耗的毛毛虫在服务器销毁"), Worm.IsValid());
            Test->TestFalse(TEXT("消耗的毛毛虫在客户端销毁"), RemoteWorm.IsValid());
            Test->TestNull(TEXT("客户端手持槽清空"), Character->GetInventoryComponent()->GetHeldItem());
            Test->TestEqual(TEXT("未交付的外卖不提前污染任务奖励"), Tasks->PreviewReward(Task).SpecialMultiplier, 1.f);
            PC->ServerRequestInteraction(Character, EDeliveryInteractionRequest::CompleteHold, RemotePackage.Get());
            {
                auto* SecondWorm = Server->SpawnActor<ADeliveryCaterpillarItem>();
                Test->TestTrue(TEXT("第二只毛毛虫可独立拾取"), HostCharacter->GetInventoryComponent()->TryPickup(SecondWorm));
                Test->TestFalse(TEXT("重复投放不能再次消耗或叠加"), Package->Pickup(HostCharacter));
                Test->TestTrue(TEXT("拒绝重复投放后毛毛虫仍在原玩家手中"), HostCharacter->GetInventoryComponent()->GetHeldItem() == SecondWorm);
                HostCharacter->GetInventoryComponent()->ConsumeHeldItem(SecondWorm);
                SecondWorm->Destroy();
            }
            Test->TestTrue(TEXT("另一玩家能拿起加过毛毛虫的外卖"), HostCharacter->GetCarryComponent()->TryReceivePackage(Package.Get(), false));
            Test->TestTrue(TEXT("换持有者保留毛毛虫状态"), Package->ContainsCaterpillar());
            Step(); break;
        case 26:
            Test->TestTrue(TEXT("客户端看到换手后仍有加成"), RemotePackage->ContainsCaterpillar());
            HostCharacter->GetInventoryComponent()->DropHeldItem();
            Package->GetItemMesh()->SetSimulatePhysics(false);
            Package->SetActorLocation(AuthorityCharacter->GetActorLocation() + FVector(0,35,-20));
            Step(); break;
        case 27:
            PC->ServerRequestInteraction(Character, EDeliveryInteractionRequest::BeginHold, RemotePackage.Get());
            Step(); break;
        case 28:
            PC->ServerRequestInteraction(Character, EDeliveryInteractionRequest::CompleteHold, RemotePackage.Get());
            Step(); break;
        case 29:
            if (!Test->TestTrue(TEXT("远端正常长按取回加成外卖"), AuthorityCharacter->GetCarriedTaskPackage() == Package.Get())) return End();
            bCaterpillarComplete = true;
            Stage = 9; NextTime = Now + .8f; break;
        default:
            Test->TestEqual(TEXT("重复交付后奖励保持不变"), UDeliveryWalletComponent::FindWallet(AuthorityCharacter)->GetBalance(), PaidBalance);
            Test->TestEqual(TEXT("奖励余额复制到交付客户端"), UDeliveryWalletComponent::FindWallet(Character)->GetBalance(), PaidBalance);
            Test->TestEqual(TEXT("结束动作持久显示"), RemoteGrandpa->GetCurrentAction(), EDeliveryGrandpaAction::Finished);
            return End();
        }
        return false;
    }
private:
    bool End() { GEditor->RequestEndPlayMap(); bEnding = true; return false; }
    FAutomationTestBase* Test;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TWeakObjectPtr<ADeliveryPackageItem> Package;
    TWeakObjectPtr<ADeliveryHandheldItem> Axe;
    TWeakObjectPtr<ADeliveryCaterpillarItem> Worm, RemoteWorm;
    TWeakObjectPtr<ADeliveryPackageItem> RemotePackage;
    bool bTestCaterpillar = false, bCaterpillarComplete = false;
    bool bSelectedAxe = false;
    bool bReacquiredPackage = false;
    int32 InitialClientBalance = 0, InitialHostBalance = 0, PaidBalance = 0;
    int32 AxeSwings = 0;
    FVector StartledOrigin = FVector::ZeroVector;
    bool bRequested = false, bEnding = false;
    int Stage = 0;
    float NextTime = 0;
    FVector StrollOrigin = FVector::ZeroVector;
    float MaxStrollDistance = 0;
    bool bTrackMovement = false;
    bool bClientSawShake = false, bHostSawShake = false;
    double Started = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryGrandpaPIETest, "Delivery.NPC.Grandpa.ListenServerFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryGrandpaPIETest::RunTest(const FString&)
{
    if (!FApp::CanEverRender()) { AddWarning(TEXT("需要渲染器才能实际运行双人 PIE，本次跳过")); return true; }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryGrandpaPIECommand(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCaterpillarPIETest, "Delivery.NPC.Grandpa.CaterpillarMultiplayer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryCaterpillarPIETest::RunTest(const FString&)
{
    if (!FApp::CanEverRender()) { AddError(TEXT("毛毛虫多人流程必须运行真实 PIE")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryGrandpaPIECommand(this, true));
    return true;
}
#endif
