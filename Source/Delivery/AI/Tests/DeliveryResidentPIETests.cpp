#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/Residents/DeliveryResidentCombatComponent.h"
#include "AI/Residents/DeliveryNpcPresentationComponent.h"
#include "AI/Residents/DeliveryConversationComponent.h"
#include "AI/Residents/DeliveryVehicleHoldComponent.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "AI/DeliveryAiGatewaySubsystem.h"
#include "AI/DeliveryAiSettings.h"
#include "UI/DeliveryHUD.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Game/DeliveryGameMode.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/EditableTextBox.h"
#include "UI/DeliveryConversationWidget.h"
#include "InputKeyEventArgs.h"
#include "GameFramework/SpringArmComponent.h"

// 三人真实 PIE 验证正式蓝图执行和复制，建议由测试注入，不宣称真实模型决策
class FDeliveryResidentPIECommand : public IAutomationLatentCommand
{
public:
    explicit FDeliveryResidentPIECommand(FAutomationTestBase* InTest, bool bInMap=false, bool bInHost=false)
        :Test(InTest),bActualMap(bInMap),bHostDriver(bInHost) {}
    bool Update() override
    {
        UWorld* Server=nullptr; TArray<UWorld*> Clients;
        for (const auto& Context:GEngine->GetWorldContexts())
            if (Context.WorldType==EWorldType::PIE && Context.World())
            { if (Context.World()->GetNetMode()==NM_Client) Clients.Add(Context.World()); else Server=Context.World(); }
        if (bEnding)
        {
            if (Server || !Clients.IsEmpty()) return false;
            GetMutableDefault<UDeliveryAiSettings>()->bEnabled=bOldEnabled; return true;
        }
        if (!bStarted)
        {
            if (Server || !Clients.IsEmpty()) { Test->AddError(TEXT("居民测试需要空闲编辑器")); return true; }
            if (!FEditorFileUtils::LoadMap(bActualMap?TEXT("/Game/Level/TestForCharacter"):TEXT("/Engine/Maps/Entry"),false,false)) { Test->AddError(TEXT("测试地图加载失败")); return true; }
            auto* World=GEditor->GetEditorWorldContext().World(); World->GetWorldSettings()->DefaultGameMode=LoadClass<ADeliveryGameMode>(nullptr,TEXT("/Game/Blueprint/GameMode/BP_DeliverGameMode.BP_DeliverGameMode_C"));
            if (!bActualMap)
            {
            auto* Floor=World->SpawnActor<AStaticMeshActor>(FVector(0,0,-50),FRotator::ZeroRotator);
            Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
            Floor->SetActorScale3D(FVector(100,100,1)); Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
            Floor->GetStaticMeshComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel3,ECR_Ignore);
            World->SpawnActor<APlayerStart>(FVector(0,0,150),FRotator::ZeroRotator);
            // 三名玩家都从居民同一侧接近，避免 T 正确选择另一名无遮挡居民
            Origin=FVector(-200,-300,0);
            }
            else Origin=FVector(11400,12650,1970);
            bOldEnabled=GetDefault<UDeliveryAiSettings>()->bEnabled; GetMutableDefault<UDeliveryAiSettings>()->bEnabled=false;
            Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(),GetTransientPackage()));
            Settings->SetPlayNetMode(PIE_ListenServer); Settings->SetPlayNumberOfClients(bActualMap?2:3); Settings->SetRunUnderOneProcess(true);
            Settings->NewWindowWidth=960; Settings->NewWindowHeight=640;
            FRequestPlaySessionParams Params; Params.EditorPlaySettings=Settings.Get(); Params.bAllowOnlineSubsystem=false;
            // 重现两个玩家使用同一个 PlayFromHere 起点，不能先传送玩家掩盖出生失败
            if (bActualMap) { Params.StartLocation=FVector(11500,12350,2100); Params.StartRotation=FRotator(0,90,0); }
            GEditor->RequestPlaySession(Params); bStarted=true; StartedAt=FPlatformTime::Seconds(); return false;
        }
        if (FPlatformTime::Seconds()-StartedAt>150) { Test->AddError(FString::Printf(TEXT("居民三人 PIE 超时 stage=%d"),Stage)); return End(); }
        if (!Server || Clients.Num()!=(bActualMap?1:2)) return false;
        auto* PC=(bHostDriver?Server:Clients[0])->GetFirstPlayerController(); if (!PC || !PC->GetPawn() || !PC->PlayerState) return false;
        const float Now=Server->GetTimeSeconds(); if (Now<NextTime) return false;
        if (Stage==0)
        {
            TArray<UWorld*> Views=Clients; Views.Add(Server);
            for (auto* ViewWorld:Views)
            {
                auto* Local=ViewWorld->GetFirstPlayerController();
                if (!Local || !Cast<ADeliveryCharacter>(Local->GetPawn())) return false;
                if (ViewWorld->GetTimeSeconds()<3) return false;
            }
            CaptureViews(Server,Clients,TEXT("spawn"));
            for (auto It=Server->GetPlayerControllerIterator();It;++It)
                if (It->Get()->PlayerState && It->Get()->PlayerState->GetPlayerId()==PC->PlayerState->GetPlayerId()) DriverPC=It->Get();
            if (!DriverPC.IsValid() || !Cast<ADeliveryCharacter>(DriverPC->GetPawn())) return false;
            FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn; Spawn.Owner=DriverPC->PlayerState;
            auto* ClassA=LoadClass<ADeliveryResidentCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"));
            auto* ClassB=LoadClass<ADeliveryResidentCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
            if (!Test->TestNotNull(TEXT("正式居民蓝图A"),ClassA) || !Test->TestNotNull(TEXT("正式居民蓝图B"),ClassB)) return End();
            if (bActualMap)
            {
                // 正式地图直接验证已保存的居民实例与站位，不另外生成贴墙的测试替身
                for (TActorIterator<ADeliveryResidentCharacter> It(Server);It;++It)
                { if (It->GetClass()==ClassA) A=*It; else if (It->GetClass()==ClassB) B=*It; }
                if (!Test->TestNotNull(TEXT("正式地图中的大嗓门"),A.Get()) || !Test->TestNotNull(TEXT("正式地图中的小算盘"),B.Get())) return End();
            }
            else
            {
                A=Server->SpawnActor<ADeliveryResidentCharacter>(ClassA,Origin+FVector(300,0,80),FRotator::ZeroRotator,Spawn);
                Spawn.Owner=A.Get(); B=Server->SpawnActor<ADeliveryResidentCharacter>(ClassB,Origin+FVector(300,-130,80),FRotator(0,180,0),Spawn);
            }
            InitialA=A->GetActorLocation();
            A->bAlwaysRelevant=true; B->bAlwaysRelevant=true;
            Test->TestTrue(TEXT("正式居民蓝图开启复制"),A->GetIsReplicated() && B->GetIsReplicated());
            Test->TestNotNull(TEXT("正式行为树配置"),A->BehaviorTree.Get());
            Test->TestEqual(TEXT("居民树使用自己的黑板"),GetNameSafe(A->BehaviorTree->BlackboardAsset),FString(TEXT("BB_Resident")));
            Test->TestNotNull(TEXT("正式谈话动画"),A->Presentation->TalkAnimation.Get());
            ++Stage; NextTime=Now+2; return false;
        }
        TArray<ADeliveryResidentCharacter*> RemoteAs,RemoteBs;
        for (auto* Client:Clients)
        {
            ADeliveryResidentCharacter* RA=nullptr; ADeliveryResidentCharacter* RB=nullptr;
            // 场景中每种正式居民恰有一名，不依赖 Owner 属性先于 Actor 到达
            for (TActorIterator<ADeliveryResidentCharacter> It(Client);It;++It)
            {
                if (It->GetClass()==A->GetClass()) RA=*It;
                if (It->GetClass()==B->GetClass()) RB=*It;
            }
            if (!RA || !RB) return false; RemoteAs.Add(RA); RemoteBs.Add(RB);
        }
        if (Stage==1)
        {
            Test->TestTrue(TEXT("正式居民在默认地图碰撞上站立"),A->GetActorLocation().Z>50 && A->GetCharacterMovement()->IsMovingOnGround());
            Test->TestFalse(TEXT("客户端不能请求 NPC 决策"),RemoteAs[0]->Agent->RequestDecision());
            auto* Conversation=PC->FindComponentByClass<UDeliveryConversationComponent>();
            if (!Test->TestNotNull(TEXT("远端控制器持有交谈组件"),Conversation)) return End();
            // 经过 T 的正式 Enhanced Input 映射打开最近居民，不直接调用打开面板 RPC
            PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::T,IE_Pressed,1));
            ++Stage; NextTime=Now+.5f; return false;
        }
        if (Stage==2)
        {
            PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::T,IE_Released,0));
            auto* HUD=Cast<ADeliveryHUD>(PC->GetHUD());
            Test->TestTrue(TEXT("交谈窗口只在发起客户端打开"),HUD && HUD->IsPanelOpen(TEXT("ResidentConversation")));
            TArray<UUserWidget*> Panels;
            UWidgetBlueprintLibrary::GetAllWidgetsOfClass(PC,Panels,UDeliveryConversationWidget::StaticClass(),true);
            Test->TestTrue(TEXT("交谈面板已经构建可见内容而非空 Slate 根"),Panels.ContainsByPredicate([PC](const auto* Widget)
            { return Widget->GetOwningPlayer()==PC && Widget->GetDesiredSize().X>100 && Widget->GetDesiredSize().Y>100; }));
            if (!bConversationSent)
            {
                for (auto* Panel:Panels)
                {
                    if (Panel->GetOwningPlayer()!=PC) continue;
                    TArray<UWidget*> Children; Panel->WidgetTree->GetAllWidgets(Children);
                    for (auto* Child:Children) if (auto* Input=Cast<UEditableTextBox>(Child)) Input->SetText(FText::FromString(TEXT("对不起，请让我先送货")));
                    // 调用发送按钮绑定的同一处理函数，覆盖文本框 → UI → 组件 → RPC 全链路
                    if (auto* Send=Panel->FindFunction(TEXT("Send"))) { Panel->ProcessEvent(Send,nullptr); bConversationSent=true; }
                }
                if (!Test->TestTrue(TEXT("通过实际交谈面板发送文本"),bConversationSent)) return End();
                NextTime=Now+.5f; return false;
            }
            if (auto* OtherHUD=Cast<ADeliveryHUD>((bHostDriver?Clients.Last():Server)->GetFirstPlayerController()->GetHUD()))
                Test->TestFalse(TEXT("旁观者不弹出交谈窗口"),OtherHUD->IsPanelOpen(TEXT("ResidentConversation")));
            CaptureViews(Server,Clients,TEXT("conversation"));
            if (HUD) HUD->CloseResidentConversation();
            const bool bHeard=A->Agent->Evidence.ContainsByPredicate([](const auto& E){ return E.Kind==TEXT("PlayerSpeech"); });
            if (!Test->TestTrue(TEXT("远端交谈 RPC 进入服务器私有记忆"),bHeard)) return End();
            A->SetActorRotation(FRotator(0,135,0));
            if (!Approve(A.Get(),EDeliveryNpcAction::LookAt,B.Get(),TEXT("先把话说清楚，再走也不迟"))) return End();
            // 转体上限 240 度/秒，135 度转向加上 BT 的领取等待，半秒不足以覆盖低帧率
            ++Stage; NextTime=Now+1.f; return false;
        }
        if (Stage==3)
        {
            CaptureViews(Server,Clients,TEXT("speech"));
            Test->TestEqual(TEXT("保存的行为树已领取动作"),A->Agent->GetPublicState().Status,EDeliveryNpcActionStatus::Running);
            Test->TestTrue(TEXT("停步看向目标时身体实际转向"),FVector::DotProduct(A->GetActorForwardVector(),(B->GetActorLocation()-A->GetActorLocation()).GetSafeNormal2D())>.3f);
            for (auto* RA:RemoteAs)
            {
                Test->TestEqual(TEXT("对白编号同步到交谈者与旁观者"),RA->Presentation->GetSpeech().TurnId,A->Presentation->GetSpeech().TurnId);
                Test->TestTrue(TEXT("双方正在播放同一段对白"),RA->Presentation->GetSpeech().bActive);
            }
            A->ReceiveImpactFrom(FVector(-30000,0,0),A->GetActorLocation(),B.Get());
            ++Stage; NextTime=Now+.5f; return false;
        }
        if (Stage==4)
        {
            CaptureViews(Server,Clients,TEXT("knockdown"));
            for (auto* RA:RemoteAs) { Test->TestTrue(TEXT("受击倒地复制"),RA->IsKnockedDown()); Test->TestFalse(TEXT("受击停止对白"),RA->Presentation->GetSpeech().bActive); }
            ++Stage; NextTime=Now+7; return false;
        }
        if (Stage==5)
        {
            Test->TestFalse(TEXT("服务器起身结束"),A->IsKnockedDown());
            for (auto* RA:RemoteAs) Test->TestFalse(TEXT("旁观端起身结束"),RA->IsKnockedDown());
            A->SetActorLocation(InitialA); B->SetActorLocation(InitialA+FVector(bActualMap?130:0,bActualMap?0:-130,0));
            if (!Approve(A.Get(),EDeliveryNpcAction::StrikeOnce,B.Get(),TEXT("别挡我的路"))) return End();
            ++Stage; NextTime=Now+1; return false;
        }
        if (Stage==6)
        {
            CaptureViews(Server,Clients,TEXT("fight"));
            Test->TestTrue(TEXT("正式 BT 挥拳实际击倒另一居民"),B->IsKnockedDown());
            for (auto* RB:RemoteBs) Test->TestTrue(TEXT("互殴结果同步旁观者"),RB->IsKnockedDown());
            ++Stage; NextTime=Now+7; return false;
        }
        if (Stage==7)
        {
            if (!bRetaliationStarted)
            {
                A->SetActorLocation(InitialA); B->SetActorLocation(InitialA+FVector(bActualMap?130:0,bActualMap?0:-130,0));
                if (!Approve(B.Get(),EDeliveryNpcAction::StrikeOnce,A.Get(),TEXT("你还敢动手？"))) return End();
                bRetaliationStarted=true; NextTime=Now+1; return false;
            }
            if (!bRetaliationChecked)
            {
                Test->TestTrue(TEXT("另一居民通过正式行为树还击"),A->IsKnockedDown());
                for (auto* RA:RemoteAs) Test->TestTrue(TEXT("还击倒地同步双方"),RA->IsKnockedDown());
                CaptureViews(Server,Clients,TEXT("retaliation"));
                bRetaliationChecked=true; NextTime=Now+7; return false;
            }
            auto* Driver=Cast<ADeliveryCharacter>(DriverPC->GetPawn()); if (!Driver || Driver->IsIncapacitated()) return false;
            auto* Class=LoadClass<ADeliveryMotorbike>(nullptr,TEXT("/Game/Vehicle/Motorbike/BP_Motorbike.BP_Motorbike_C"));
            FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Bike=Server->SpawnActor<ADeliveryMotorbike>(Class,Driver->GetActorLocation()+FVector(120,0,0),FRotator::ZeroRotator,Spawn);
            if (!Test->TestTrue(TEXT("远端玩家进入正式电瓶车"),Bike->TryEnter(Driver))) return End();
            ++Stage; NextTime=Now+1; return false;
        }
        auto* RemoteBike=Cast<ADeliveryMotorbike>(PC->GetPawn()); if (Stage<=10 && !RemoteBike) return false;
        if (Stage==8)
        {
            A->SetActorLocation(Bike->ResidentHold->GetHoldLocation()); A->GetCharacterMovement()->StopMovementImmediately();
            if (!Approve(A.Get(),EDeliveryNpcAction::HoldVehicle,Bike.Get(),TEXT("等等，把话说完"))) return End();
            ++Stage; NextTime=Now+.5f; return false;
        }
        if (Stage==9)
        {
            CaptureViews(Server,Clients,TEXT("hold"));
            Test->TestTrue(TEXT("车辆权威占用有效"),Bike->ResidentHold->IsHeld());
            Test->TestTrue(TEXT("驾驶客户端看到拽车占用"),RemoteBike->ResidentHold->IsHeld());
            Test->TestTrue(TEXT("驾驶预测忽略自己的拽车居民"),CastChecked<UPrimitiveComponent>(RemoteBike->GetRootComponent())->GetMoveIgnoreActors().Contains(bHostDriver?A.Get():RemoteAs[0]));
            Test->TestEqual(TEXT("驾驶预测读取相同减速倍率"),RemoteBike->ResidentHold->GetSpeedScale(),.4f);
            Test->TestFalse(TEXT("隐藏驾驶员在客户端也关闭碰撞"),RemoteBike->GetDriver()->GetActorEnableCollision());
            Test->TestEqual(TEXT("居民胶囊不遮挡驾驶镜头"),RemoteAs[0]->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Camera),ECR_Ignore);
            auto* Boom=RemoteBike->FindComponentByClass<USpringArmComponent>();
            Test->TestTrue(TEXT("空旷车尾镜头不会缩进骑手身体"),Boom && FVector::Dist(Boom->GetComponentLocation(),PC->PlayerCameraManager->GetCameraLocation())>200);
            PC->SetControlRotation(FRotator(-12,0,0)); RemoteBike->MoveInputRaw=FVector2D(0,1);
            ++Stage; NextTime=Now+2.7f; return false;
        }
        if (Stage==10)
        {
            CaptureViews(Server,Clients,TEXT("release"));
            RemoteBike->MoveInputRaw=FVector2D::ZeroVector;
            Test->TestFalse(TEXT("真实远端驾驶输入挣脱"),Bike->ResidentHold->IsHeld());
            Test->TestEqual(TEXT("实际解除原因是持续油门，而非超时或碰撞中断"),A->Agent->GetPublicState().Reason,FName(TEXT("DriverEscaped")));
            Test->TestFalse(TEXT("挣脱结果复制到驾驶端"),RemoteBike->ResidentHold->IsHeld());
            Test->TestFalse(TEXT("驾驶端清理拽车碰撞忽略"),CastChecked<UPrimitiveComponent>(RemoteBike->GetRootComponent())->GetMoveIgnoreActors().Contains(bHostDriver?A.Get():RemoteAs[0]));
            Test->TestNull(TEXT("居民不会残留车辆关联"),A->GetHeldVehicle());
            for (auto* RA:RemoteAs) Test->TestTrue(TEXT("居民位置与旁观端一致"),FVector::Dist(RA->GetActorLocation(),A->GetActorLocation())<35.f);
            Bike->ExitVehicle(); ++Stage; NextTime=Now+3; return false;
        }
        if (Stage==11 || Stage==13)
        {
            auto* Driver=Cast<ADeliveryCharacter>(PC->GetPawn());
            if (!Test->TestNotNull(TEXT("下车或销毁车辆后恢复原角色控制"),Driver)) return End();
            Test->TestTrue(TEXT("恢复步行碰撞"),Driver->GetActorEnableCollision());
            Test->TestFalse(TEXT("恢复步行角色显示"),Driver->IsHidden());
            CaptureViews(Server,Clients,Stage==11?TEXT("exit"):TEXT("destroy"));
            if (Stage==11)
            {
                if (!Test->TestTrue(TEXT("下车后再次上车"),Bike->TryEnter(Cast<ADeliveryCharacter>(DriverPC->GetPawn())))) return End();
                ++Stage; NextTime=Now+1; return false;
            }
            A->Destroy(); B->Destroy();
            Test->AddInfo(TEXT("正式蓝图：T 交谈、BT 双向互殴、驾驶挣脱、下车与销毁清理完成")); return End();
        }
        if (Stage==12)
        {
            Bike->Destroy(); ++Stage; NextTime=Now+3; return false;
        }
        return false;
    }
private:
    // 读取每个本地玩家实际渲染的视口，并验证镜头仍属于对应的受控 Pawn
    void CaptureViews(UWorld* Server,const TArray<UWorld*>& Clients,const TCHAR* Phase)
    {
        TArray<UWorld*> Worlds=Clients; Worlds.Insert(Server,0);
        for (int32 Index=0;Index<Worlds.Num();++Index)
        {
            auto* PC=Worlds[Index]->GetFirstPlayerController();
            if (!Test->TestNotNull(TEXT("视口控制器"),PC) || !Test->TestTrue(TEXT("各端拥有受控角色"),IsValid(PC->GetPawn()))) continue;
            Test->TestTrue(TEXT("相机跟随当前受控角色"),PC->GetViewTarget()==PC->GetPawn());
            auto* Camera=PC->PlayerCameraManager.Get();
            if (!Test->TestNotNull(TEXT("各端相机已创建"),Camera)) continue;
            Test->TestTrue(TEXT("相机不在世界原点或远离角色"),FVector::Dist(Camera->GetCameraLocation(),PC->GetPawn()->GetActorLocation())<1200);
            Test->TestTrue(TEXT("相机没有倒转"),FMath::Abs(Camera->GetCameraRotation().Roll)<1);
            Test->AddInfo(FString::Printf(TEXT("View phase=%s world=%s pawn=%s pos=%s camera=%s rotation=%s"),Phase,*Worlds[Index]->GetName(),*GetNameSafe(PC->GetPawn()),*PC->GetPawn()->GetActorLocation().ToString(),*Camera->GetCameraLocation().ToString(),*Camera->GetCameraRotation().ToString()));
            if (auto* Boom=PC->GetPawn()->FindComponentByClass<USpringArmComponent>())
            {
                FHitResult Hit; FCollisionQueryParams Query(SCENE_QUERY_STAT(ResidentCameraTest),false,PC->GetPawn());
                Worlds[Index]->SweepSingleByChannel(Hit,Boom->GetComponentLocation()+Boom->TargetOffset,Boom->GetUnfixedCameraPosition(),FQuat::Identity,Boom->ProbeChannel,FCollisionShape::MakeSphere(Boom->ProbeSize),Query);
                Test->AddInfo(FString::Printf(TEXT("CameraProbe phase=%s world=%d hit=%s component=%s arm=%.1f"),Phase,Index,*GetNameSafe(Hit.GetActor()),*GetNameSafe(Hit.GetComponent()),Boom->TargetArmLength));
            }
            auto* Local=PC->GetLocalPlayer(); FViewport* Viewport=Local && Local->ViewportClient?Local->ViewportClient->Viewport:nullptr;
            TArray<FColor> Pixels;
            if (!Test->TestTrue(TEXT("读取实际游戏视口"),Viewport && Viewport->ReadPixels(Pixels))) continue;
            const FIntPoint Size=Viewport->GetSizeXY(); TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,Png);
            const FString Folder=FPaths::ProjectSavedDir()/TEXT("Automation/ResidentViews")/(bActualMap?(bHostDriver?TEXT("MapHost"):TEXT("MapClient")):TEXT("FlatThree"));
            IFileManager::Get().MakeDirectory(*Folder,true);
            Test->TestTrue(TEXT("保存游戏视口证据"),FFileHelper::SaveArrayToFile(Png,*(Folder/FString::Printf(TEXT("%s-%s%d.png"),Phase,Index==0?TEXT("host"):TEXT("client"),Index))));
            // Scene 读回不包含 Slate/UMG，窗口截图另外保存交谈面板和字幕
            auto Window=Local->ViewportClient->GetWindow(); FIntVector WindowSize;
            if (Window.IsValid() && FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(),Pixels,WindowSize))
            {
                FImageUtils::PNGCompressImageArray(WindowSize.X,WindowSize.Y,Pixels,Png);
                Test->TestTrue(TEXT("保存带 HUD 的窗口证据"),FFileHelper::SaveArrayToFile(Png,*(Folder/FString::Printf(TEXT("%s-%s%d-ui.png"),Phase,Index==0?TEXT("host"):TEXT("client"),Index))));
            }
        }
    }
    bool Approve(ADeliveryResidentCharacter* Resident,EDeliveryNpcAction Action,AActor* Target,const FString& Speech)
    {
        Resident->Agent->CancelPlan(TEXT("TestNextScenario"));
        if (Target) Resident->ObserveFact(TEXT("TestVisibleTarget"),Target,TEXT("测试场景已感知目标"),EDeliveryNpcEvidenceSource::Seen);
        auto* Config=GetMutableDefault<UDeliveryAiSettings>(); Config->bEnabled=true;
        const bool bQueued=Resident->Agent->RequestDecision(); Config->bEnabled=false;
        if (!Test->TestTrue(TEXT("测试建议进入正式请求流程"),bQueued)) return false;
        const auto Request=Resident->Agent->ActiveRequest;
        Resident->GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(Resident->Agent);
        const auto* C=Request.Candidates.FindByPredicate([&](const auto& Candidate){ return Candidate.Action==Action && (!Target || Resident->Agent->KnownSubjects.FindRef(Candidate.TargetId)==Target); });
        if (!Test->TestNotNull(TEXT("动作和目标必须存在于服务器候选"),C)) return false;
        FDeliveryNpcDecisionProposal P; P.WorldEpoch=Request.WorldEpoch; P.NpcId=Request.NpcId; P.RequestId=Request.RequestId;
        P.ContextRevision=Request.ContextRevision; P.PlanRevision=Request.PlanRevision; P.Action=Action; P.TargetId=C->TargetId; P.EvidenceIds=C->EvidenceIds;
        P.Duration=Action==EDeliveryNpcAction::StrikeOnce?1.2f:4.f; P.Speech=Speech; P.Emotion=TEXT("Angry");
        Resident->Agent->ReceiveDecision(P.RequestId,&P,NAME_None);
        return Test->TestEqual(TEXT("建议通过正式审批"),Resident->Agent->GetPublicState().Status,EDeliveryNpcActionStatus::Accepted);
    }
    bool End() { GEditor->RequestEndPlayMap(); bEnding=true; return false; }
    FAutomationTestBase* Test;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TWeakObjectPtr<ADeliveryResidentCharacter> A,B;
    TWeakObjectPtr<ADeliveryMotorbike> Bike;
    TWeakObjectPtr<APlayerController> DriverPC;
    double StartedAt=0;
    float NextTime=0;
    int32 Stage=0;
    bool bStarted=false,bEnding=false,bOldEnabled=false;
    bool bActualMap=false,bHostDriver=false;
    bool bRetaliationStarted=false,bRetaliationChecked=false;
    bool bConversationSent=false;
    FVector Origin=FVector::ZeroVector;
    FVector InitialA=FVector::ZeroVector;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryResidentPIETest,"Delivery.AI.ResidentMultiplayer",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryResidentPIETest::RunTest(const FString&)
{
    if (!FApp::CanEverRender()) { AddError(TEXT("居民多人测试需要真实渲染 PIE")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryResidentPIECommand(this)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryResidentMapClientTest,"Delivery.AI.ResidentMap.Client",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryResidentMapClientTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryResidentPIECommand(this,true,false)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryResidentMapHostTest,"Delivery.AI.ResidentMap.Host",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryResidentMapHostTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryResidentPIECommand(this,true,true)); return true;
}
#endif
