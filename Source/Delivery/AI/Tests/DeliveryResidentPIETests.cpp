#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/PointLight.h"
#include "Components/PointLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "TextureResource.h"
#include "Curves/CurveFloat.h"
#include "AI/Presentation/DeliveryNpcExpressionSet.h"
#include "AI/Presentation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/DeliveryNpcLookComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "UI/DeliveryHUD.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
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
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
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
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "UI/DeliveryConversationWidget.h"
#include "InputKeyEventArgs.h"
#include "GameFramework/SpringArmComponent.h"

// 使用三人 PIE 检查正式蓝图和多人同步，动作建议由测试代码提供
class FDeliveryResidentPIECommand : public IAutomationLatentCommand
{
public:
    explicit FDeliveryResidentPIECommand(FAutomationTestBase* InTest, bool bInMap=false, bool bInHost=false,FString InLiveEndpoint=FString(),bool bInFacePreviewOnly=false,bool bInCompleteSpeech=false,bool bInPlayerGlance=false)
        :Test(InTest),LiveEndpoint(MoveTemp(InLiveEndpoint)),bFacePreviewOnly(bInFacePreviewOnly),bCompleteSpeech(bInCompleteSpeech),bPlayerGlance(bInPlayerGlance),bActualMap(bInMap),bHostDriver(bInHost) {}
    ~FDeliveryResidentPIECommand() { RestoreSettings(); }
    bool Update() override
    {
        UWorld* Server=nullptr; TArray<UWorld*> Clients;
        for (const auto& Context:GEngine->GetWorldContexts())
            if (Context.WorldType==EWorldType::PIE && Context.World())
            { if (Context.World()->GetNetMode()==NM_Client) Clients.Add(Context.World()); else Server=Context.World(); }
        if (bEnding)
        {
            if (Server || !Clients.IsEmpty()) return false;
            RestoreSettings(); return true;
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
            // 基础地图没有灯光，在测试场景中补光以检查人物和表情
            auto* Sun=World->SpawnActor<ADirectionalLight>(FVector(0,0,600),FRotator(-45,-40,0));
            Sun->GetLightComponent()->SetIntensity(4.f);
            auto* Sky=World->SpawnActor<ASkyLight>();
            Sky->GetLightComponent()->SetIntensity(1.f);
            Sky->GetLightComponent()->RecaptureSky();
            }
            else Origin=FVector(11400,12650,1970);
            auto* Config=GetMutableDefault<UDeliveryAiSettings>();
            bOldEnabled=Config->bEnabled; bOldStreaming=Config->bStreamReplies;
            OldEndpoint=Config->DecisionEndpoint; OldTimeout=Config->RequestTimeout; bChangedSettings=true;
            Config->bEnabled=false;
            if (!LiveEndpoint.IsEmpty() || bCompleteSpeech)
            {
                if (!LiveEndpoint.IsEmpty()) Config->DecisionEndpoint=LiveEndpoint;
                Config->bStreamReplies=true; Config->RequestTimeout=30;
            }
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
        const float Now=Server->GetTimeSeconds();
        if (ImpactStartedAt>=0 && Now-ImpactStartedAt<=1.5f) TraceImpact(Server,Clients);
        if (Now<NextTime) return false;
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
            auto* ClassA=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"));
            auto* ClassB=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
            if (!Test->TestNotNull(TEXT("正式居民蓝图A"),ClassA) || !Test->TestNotNull(TEXT("正式居民蓝图B"),ClassB)) return End();
            if (bActualMap)
            {
                // 正式地图直接验证已保存的居民实例与站位，不另外生成贴墙的测试替身
                for (TActorIterator<ADeliveryNpcCharacter> It(Server);It;++It)
                { if (It->GetClass()==ClassA) A=*It; else if (It->GetClass()==ClassB) B=*It; }
                if (!Test->TestNotNull(TEXT("正式地图中的大军"),A.Get()) || !Test->TestNotNull(TEXT("正式地图中的小芳"),B.Get())) return End();
            }
            else
            {
                A=Server->SpawnActor<ADeliveryNpcCharacter>(ClassA,Origin+FVector(300,0,80),FRotator::ZeroRotator,Spawn);
                Spawn.Owner=A.Get(); B=Server->SpawnActor<ADeliveryNpcCharacter>(ClassB,Origin+FVector(300,-130,80),FRotator(0,180,0),Spawn);
            }
            InitialA=A->GetActorLocation();
            A->bAlwaysRelevant=true; B->bAlwaysRelevant=true;
            Test->TestTrue(TEXT("正式居民蓝图开启复制"),A->GetIsReplicated() && B->GetIsReplicated());
            Test->TestNotNull(TEXT("正式行为树配置"),A->BehaviorTree.Get());
            Test->TestEqual(TEXT("居民树使用自己的黑板"),GetNameSafe(A->BehaviorTree->BlackboardAsset),FString(TEXT("BB_Resident")));
            Test->TestNotNull(TEXT("正式谈话动画"),A->Presentation->TalkAnimation.Get());
            ++Stage; NextTime=Now+2; return false;
        }
        TArray<ADeliveryNpcCharacter*> RemoteAs,RemoteBs;
        for (auto* Client:Clients)
        {
            ADeliveryNpcCharacter* RA=nullptr; ADeliveryNpcCharacter* RB=nullptr;
            // 场景中每种正式居民恰有一名，不依赖 Owner 属性先于 Actor 到达
            for (TActorIterator<ADeliveryNpcCharacter> It(Client);It;++It)
            {
                if (It->GetClass()==A->GetClass()) RA=*It;
                if (It->GetClass()==B->GetClass()) RB=*It;
            }
            if (!RA || !RB) return false; RemoteAs.Add(RA); RemoteBs.Add(RB);
        }
        if (bPlayerGlance) return UpdatePlayerGlance(Server,Clients,RemoteAs);
        if (bCompleteSpeech) return UpdateCompleteSpeech(Server,RemoteAs);
        if (bFacePreviewOnly)
        {
            if (FacePoseIndex==INDEX_NONE)
            {
                FacePoseIndex=0;
                Test->AddInfo(TEXT("仅检查正式角色的表情预设，本次不发送模型请求"));
            }
            return UpdateFacePreview(Server,Clients);
        }
        if (!LiveEndpoint.IsEmpty() && bConversationSent) return UpdateLiveConversation(Server,Clients,RemoteAs,PC);
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
                if (!LiveEndpoint.IsEmpty()) GetMutableDefault<UDeliveryAiSettings>()->bEnabled=true;
                for (auto* Panel:Panels)
                {
                    if (Panel->GetOwningPlayer()!=PC) continue;
                    TArray<UWidget*> Children; Panel->WidgetTree->GetAllWidgets(Children);
                    for (auto* Child:Children) if (auto* Input=Cast<UEditableTextBox>(Child)) Input->SetText(FText::FromString(TEXT("对不起，请让我先送货")));
                    if (LiveEndpoint.IsEmpty())
                    {
                        if (auto* Send=Panel->FindFunction(TEXT("Send")))
                        { Panel->ProcessEvent(Send,nullptr); bConversationSent=true; }
                    }
                    else
                    {
                        if (!CheckConversationPanel(Panel)) return End();
                        if (!ClickConversationButton(Panel,TEXT("ApologizeButton"))) return End();
                        bConversationSent=true;
                        LiveSubmittedAt=FPlatformTime::Seconds();
                        LiveMessage=PC->FindComponentByClass<UDeliveryConversationComponent>()->GetLastSubmissionId();
                        Test->AddInfo(FString::Printf(TEXT("LiveModel submitted round=0 speech=%s button=Apologize"),*LiveMessage.ToString()));
                    }
                }
                if (!Test->TestTrue(TEXT("通过实际交谈面板发送文本"),bConversationSent)) return End();
                NextTime=Now+(LiveEndpoint.IsEmpty()?.5f:0.f); return false;
            }
            if (auto* OtherHUD=Cast<ADeliveryHUD>((bHostDriver?Clients.Last():Server)->GetFirstPlayerController()->GetHUD()))
                Test->TestFalse(TEXT("旁观者不弹出交谈窗口"),OtherHUD->IsPanelOpen(TEXT("ResidentConversation")));
            CaptureViews(Server,Clients,TEXT("conversation"));
            if (HUD) HUD->CloseResidentConversation();
            const bool bHeard=A->Agent->Evidence.ContainsByPredicate([](const auto& E){ return E.Kind==TEXT("PlayerSpeech"); });
            if (!Test->TestTrue(TEXT("服务器记录了客户端玩家的发言"),bHeard)) return End();
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
                Test->TestTrue(TEXT("远端播放服务器分发的真实语音"),RA->Presentation->IsRecordedVoicePlaying());
            }
            Test->TestTrue(TEXT("主机播放同一语音"),A->Presentation->IsRecordedVoicePlaying());
            A->ReceiveImpactFrom(FVector(-30000,0,0),A->GetActorLocation(),B.Get());
            ImpactStartedAt=Server->GetTimeSeconds(); ImpactRealStartedAt=FPlatformTime::Seconds(); ImpactFrame=GFrameCounter;
            TraceImpact(Server,Clients);
            ++Stage; NextTime=Now+.5f; return false;
        }
        if (Stage==4)
        {
            CaptureViews(Server,Clients,TEXT("knockdown"));
            for (auto* RA:RemoteAs)
            {
                Test->TestTrue(TEXT("受击倒地复制"),RA->IsKnockedDown());
                Test->TestFalse(TEXT("受击停止对白"),RA->Presentation->GetSpeech().bActive);
                Test->TestFalse(TEXT("受击停止本机字幕"),RA->Presentation->GetDisplayedSpeech().bActive);
                Test->TestFalse(TEXT("受击停止实际声音"),RA->Presentation->IsRecordedVoicePlaying());
            }
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
            FVector BikeStart=Driver->GetActorLocation()+FVector(120,0,0);
            if (bActualMap && !PrepareDriveLayout(Server,Clients,Driver,Class,BikeStart)) return End();
            Bike=Server->SpawnActor<ADeliveryMotorbike>(Class,BikeStart,FRotator::ZeroRotator,Spawn);
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
    void PlaceGlancePlayer(UWorld* Server,const TArray<UWorld*>& Clients,int32 PlayerId,const FVector& Offset)
    {
        TArray<UWorld*> Views=Clients;Views.Insert(Server,0);
        for (UWorld* View:Views)
            for (TActorIterator<ADeliveryCharacter> It(View);It;++It)
            {
                const auto* State=It->GetPlayerState();
                if (!State || State->GetPlayerId()!=PlayerId) continue;
                It->GetActiveRagdoll()->StopRagdoll();
                const FVector Position(A->GetActorLocation().X+Offset.X,A->GetActorLocation().Y+Offset.Y,
                    It->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+2);
                const FRotator Facing(0,(A->GetActorLocation()-Position).Rotation().Yaw,0);
                It->SetActorLocationAndRotation(Position,Facing,false,nullptr,ETeleportType::TeleportPhysics);
                It->GetActiveRagdoll()->StartRagdoll();
                if (auto* PC=Cast<APlayerController>(It->GetController()); PC && PC->IsLocalController())
                    PC->SetControlRotation(FRotator(-12,Facing.Yaw,0));
            }
    }
    bool UpdatePlayerGlance(UWorld* Server,const TArray<UWorld*>& Clients,const TArray<ADeliveryNpcCharacter*>& RemoteAs)
    {
        const float Now=Server->GetTimeSeconds();
        TArray<ADeliveryNpcCharacter*> Views=RemoteAs;Views.Insert(A.Get(),0);
        if (GlanceStage==0)
        {
            Test->AddInfo(TEXT("注视检查使用正式玩家、NPC与动画蓝图；只安排玩家站位，不直接设置注视状态，不请求模型。此检查不能代替自然走路时的手感验收"));
            A->Agent->CancelPlan(TEXT("GlanceTest")); B->Agent->CancelPlan(TEXT("GlanceTest"));
            B->SetActorLocation(A->GetActorLocation()+FVector(-2000,0,0));
            int32 Index=0;
            for (auto It=Server->GetPlayerControllerIterator();It;++It)
                if (const auto* State=It->Get()->PlayerState.Get())
                    PlaceGlancePlayer(Server,Clients,State->GetPlayerId(),FVector(900,Index++*220,0));
            // 出生时可能已看过玩家，保持距离让这次经过的冷却结束
            GlanceStage=1; NextTime=Now+6.5f; return false;
        }
        if (GlanceStage==1)
        {
            auto* PlayerState=GlanceRound==0?Server->GetFirstPlayerController()->PlayerState.Get():DriverPC->PlayerState.Get();
            if (!Test->TestNotNull(TEXT("经过玩家的身份"),PlayerState)) return End();
            GlancePlayerId=PlayerState->GetPlayerId();
            GlanceHeads.Reset(); GlanceEyes.Reset(); GlanceTargets.Reset(); GlanceHeadBefore.Reset();
            for (auto* Npc:Views) GlanceHeadBefore.Add(Npc->GetMesh()->GetSocketTransform(TEXT("Head")).GetRotation());
            PlaceGlancePlayer(Server,Clients,GlancePlayerId,FVector(180,GlanceRound==0?220:-220,0));
            GlanceStartedAt=Now; GlanceStage=2; NextTime=Now+.05f; return false;
        }
        if (GlanceStage==2)
        {
            bool bAllWatching=true;
            float HostEyes=0;
            for (int32 Index=0;Index<Views.Num();++Index)
            {
                auto* Npc=Views[Index];
                auto* Target=Npc->Look?Cast<APawn>(Npc->Look->GetLookTarget()):nullptr;
                const bool bExpected=Target && Target->GetPlayerState() && Target->GetPlayerState()->GetPlayerId()==GlancePlayerId;
                bAllWatching&=bExpected;
                auto* Animation=Cast<UDeliveryNpcConversationAnimInstance>(Npc->GetMesh()->GetPostProcessInstance());
                const float HeadAngle=FMath::RadiansToDegrees(Npc->GetMesh()->GetSocketTransform(TEXT("Head")).GetRotation().AngularDistance(GlanceHeadBefore[Index]));
                const auto* Expressions=Npc->Presentation->Expressions.Get();
                const float Eyes=Expressions?Npc->GetMesh()->GetMorphTarget(Expressions->LookLeftMorph)+Npc->GetMesh()->GetMorphTarget(Expressions->LookRightMorph):0;
                if (Index==0) HostEyes=Eyes;
                if (bExpected) GlanceTargets.Add(Index);
                if (bExpected && Animation && Animation->LookWeight>.2f && HeadAngle>5) GlanceHeads.Add(Index);
                if (bExpected && Eyes>.03f) GlanceEyes.Add(Index);
                if (Animation && Animation->LookWeight>.2f)
                    Test->TestTrue(TEXT("短看不混入交谈身体前倾"),Animation->PostureWeight<.05f);
            }
            if (bAllWatching && GlanceHeads.Num()==Views.Num() && GlanceEyes.Num()==Views.Num() && HostEyes>.03f)
            {
                Test->AddInfo(FString::Printf(TEXT("PlayerGlance round=%d player=%d target_views=%d head_views=%d eye_views=%d elapsed=%.3f"),
                    GlanceRound,GlancePlayerId,GlanceTargets.Num(),GlanceHeads.Num(),GlanceEyes.Num(),Now-GlanceStartedAt));
                CaptureViews(Server,Clients,GlanceRound==0?TEXT("glance-host"):TEXT("glance-client"));
                CaptureFace(A.Get(),GlanceRound==0?TEXT("GlanceHost"):TEXT("GlanceClient"),0,.45f);
                GlanceStage=3; NextTime=Now+1.7f; return false;
            }
            if (Now-GlanceStartedAt>1.6f)
            {
                Test->AddError(FString::Printf(TEXT("三端注视未同时完成：round=%d target=%d head=%d eye=%d"),
                    GlanceRound,GlanceTargets.Num(),GlanceHeads.Num(),GlanceEyes.Num()));
                CaptureFace(A.Get(),TEXT("GlanceFailure"),0,.45f);
                return End();
            }
            return false;
        }
        for (auto* Npc:Views)
        {
            Test->TestNull(TEXT("经过玩家仍在附近时，短看也会按时结束"),Npc->Look?Npc->Look->GetLookTarget():nullptr);
            const auto* Animation=Cast<UDeliveryNpcConversationAnimInstance>(Npc->GetMesh()->GetPostProcessInstance());
            Test->TestTrue(TEXT("三端头部注视权重恢复"),Animation && Animation->LookWeight<.05f);
            const auto* Expressions=Npc->Presentation->Expressions.Get();
            if (Expressions)
                Test->TestTrue(TEXT("三端眼神偏移恢复"),Npc->GetMesh()->GetMorphTarget(Expressions->LookLeftMorph)+Npc->GetMesh()->GetMorphTarget(Expressions->LookRightMorph)<.1f);
        }
        if (GlanceRound++==0)
        {
            PlaceGlancePlayer(Server,Clients,GlancePlayerId,FVector(900,-300,0));
            GlanceStage=1; NextTime=Now+.4f; return false;
        }
        return End();
    }
    bool PrepareDriveLayout(UWorld* Server,const TArray<UWorld*>& Clients,ADeliveryCharacter* Driver,UClass* BikeClass,FVector& BikeStart)
    {
        if (!Test->TestNotNull(TEXT("正式电瓶车蓝图"),BikeClass)) return false;
        const auto* Defaults=BikeClass->GetDefaultObject<ADeliveryMotorbike>();
        const auto* Body=CastChecked<UPrimitiveComponent>(Defaults->GetRootComponent());
        FCollisionQueryParams Query(SCENE_QUERY_STAT(ResidentDriveLayout),false,Driver);
        Query.AddIgnoredActor(A.Get());
        FCollisionObjectQueryParams GroundObjects;GroundObjects.AddObjectTypesToQuery(ECC_WorldStatic);GroundObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
        auto GroundAt=[&](FVector Point,FVector& Ground)
        {
            FHitResult Hit;
            if (!Server->LineTraceSingleByObjectType(Hit,Point+FVector(0,0,500),Point-FVector(0,0,700),GroundObjects,Query) || Hit.ImpactNormal.Z<.9f)
            {
                Test->AddError(FString::Printf(TEXT("驾驶检查位置没有平缓地面：point=%s actor=%s component=%s normal=%s"),
                    *Point.ToString(),*GetNameSafe(Hit.GetActor()),*GetNameSafe(Hit.GetComponent()),*Hit.ImpactNormal.ToString()));
                return false;
            }
            Ground=Hit.ImpactPoint;return true;
        };
        // 两种驾驶身份使用之前客户端通过的同一段道路，旁观玩家站在车后
        FVector SpectatorGround;
        if (!GroundAt(Origin+FVector(100,-308,0),SpectatorGround) || !GroundAt(Origin+FVector(420,-308,0),BikeStart)) return false;
        BikeStart.Z+=Defaults->HoverHeight;
        TArray<UWorld*> Views=Clients;Views.Add(Server);
        const int32 DriverId=Driver->GetPlayerState()->GetPlayerId();
        for (UWorld* View:Views)
            for (TActorIterator<ADeliveryCharacter> It(View);It;++It)
            {
                const auto* PlayerState=It->GetPlayerState();
                if (!PlayerState || PlayerState->GetPlayerId()==DriverId) continue;
                // 沿用物理测试的放置方式，同时重建各端身体，避免旧姿态把玩家拉回车道
                It->GetActiveRagdoll()->StopRagdoll();
                const FVector Position=SpectatorGround+FVector(0,0,It->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+2);
                It->SetActorLocationAndRotation(Position,FRotator::ZeroRotator,false,nullptr,ETeleportType::TeleportPhysics);
                It->GetActiveRagdoll()->StartRagdoll();
                UE_LOG(LogTemp,Log,TEXT("NpcDriveLayout spectator world=%s player=%d position=%s"),*View->GetName(),PlayerState->GetPlayerId(),*Position.ToString());
            }
        const auto* Capsule=A->GetCapsuleComponent();
        FVector PreviousBike,PreviousNpc;
        // 检查持续油门两秒内经过的道路，保留车辆和 NPC 的实际碰撞形状与响应
        for (int32 Step=0;Step<=12;++Step)
        {
            FVector BikeGround,NpcGround;
            const FVector Point=BikeStart+FVector(Step*100,0,0);
            if (!GroundAt(Point,BikeGround) || !GroundAt(Point-FVector(145,0,0),NpcGround)) return false;
            const FVector BikePoint=BikeGround+FVector(0,0,Defaults->HoverHeight);
            const FVector NpcPoint=NpcGround+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()+2);
            if (Step==0) { PreviousBike=BikePoint;PreviousNpc=NpcPoint; }
            FHitResult Hit;
            const bool bBikeBlocked=Server->SweepSingleByChannel(Hit,PreviousBike,BikePoint,FQuat::Identity,
                Body->GetCollisionObjectType(),Body->GetCollisionShape(),Query,FCollisionResponseParams(Body->GetCollisionResponseToChannels()));
            const bool bBlocked=bBikeBlocked || Server->SweepSingleByChannel(Hit,PreviousNpc,NpcPoint,FQuat::Identity,
                Capsule->GetCollisionObjectType(),Capsule->GetCollisionShape(),Query,FCollisionResponseParams(Capsule->GetCollisionResponseToChannels()));
            if (bBlocked)
            {
                Test->AddError(FString::Printf(TEXT("驾驶测试路线被阻挡：step=%d target=%s actor=%s component=%s normal=%s"),Step,
                    bBikeBlocked?TEXT("vehicle"):TEXT("NPC"),*GetNameSafe(Hit.GetActor()),*GetNameSafe(Hit.GetComponent()),*Hit.ImpactNormal.ToString()));
                return false;
            }
            PreviousBike=BikePoint;PreviousNpc=NpcPoint;
        }
        UE_LOG(LogTemp,Log,TEXT("NpcDriveLayout ready driver=%d start=%s direction=+X checked_cm=1200 spectator=%s"),
            DriverId,*BikeStart.ToString(),*SpectatorGround.ToString());
        return true;
    }
    void TraceImpact(UWorld* Server,const TArray<UWorld*>& Clients)
    {
        if (!A.IsValid()) return;
        // 记录受击后每帧实际收到的状态，区分网络等待、低帧率和台词查询错误
        const double WorldMs=(Server->GetTimeSeconds()-ImpactStartedAt)*1000;
        const double RealMs=(FPlatformTime::Seconds()-ImpactRealStartedAt)*1000;
        const int32 Frames=int32(GFrameCounter-ImpactFrame);
        TArray<UWorld*> Views;Views.Add(Server);Views.Append(Clients);
        for (int32 Index=0;Index<Views.Num();++Index)
        {
            auto* World=Views[Index];ADeliveryNpcCharacter* Npc=Index==0?A.Get():nullptr;
            if (!Npc)
                for (TActorIterator<ADeliveryNpcCharacter> It(World);It;++It)
                    if (It->GetClass()==A->GetClass()) { Npc=*It; break; }
            if (!Npc) continue;
            const auto* P=Npc->Presentation.Get();
            UE_LOG(LogTemp,Log,TEXT("NpcImpactProbe view=%d world_ms=%.1f real_ms=%.1f frames=%d down=%d speech=%d displayed=%d playing=%d turn=%s"),
                Index,WorldMs,RealMs,Frames,Npc->IsKnockedDown(),P->GetSpeech().bActive,P->GetDisplayedSpeech().bActive,
                P->IsRecordedVoicePlaying(),*P->GetSpeech().TurnId.ToString());
            const auto* Driver=World->GetNetDriver();if (!Driver) continue;
            TArray<UNetConnection*> Connections;
            if (Driver->ServerConnection) Connections.Add(Driver->ServerConnection);
            for (UNetConnection* Connection:Driver->ClientConnections) Connections.Add(Connection);
            for (int32 ConnectionIndex=0;ConnectionIndex<Connections.Num();++ConnectionIndex)
            {
                const auto* Connection=Connections[ConnectionIndex];
                UE_LOG(LogTemp,Log,TEXT("NpcImpactNet view=%d connection=%d world_ms=%.1f real_ms=%.1f frames=%d speed=%d queued_bits=%d out_bps=%d in_bps=%d out_total=%d in_total=%d out_lost=%d in_lost=%d"),
                    Index,ConnectionIndex,WorldMs,RealMs,Frames,Connection->CurrentNetSpeed,Connection->QueuedBits,
                    Connection->OutBytesPerSecond,Connection->InBytesPerSecond,Connection->OutTotalBytes,Connection->InTotalBytes,
                    Connection->OutTotalPacketsLost,Connection->InTotalPacketsLost);
            }
        }
    }
    void RestoreFaceSettings()
    {
        for (const auto& Pair:OriginalFaceSettings)
            if (Pair.Key.IsValid() && Pair.Value.IsValid()) Pair.Key->Presentation->Expressions=Pair.Value.Get();
        OriginalFaceSettings.Reset();
    }
    void RestoreSettings()
    {
        RestoreFaceSettings();
        if (!bChangedSettings) return;
        auto* Config=GetMutableDefault<UDeliveryAiSettings>();
        Config->bEnabled=bOldEnabled; Config->bStreamReplies=bOldStreaming;
        Config->DecisionEndpoint=OldEndpoint; Config->RequestTimeout=OldTimeout;
        bChangedSettings=false;
    }
    bool UpdateCompleteSpeech(UWorld* Server,const TArray<ADeliveryNpcCharacter*>& RemoteAs)
    {
        const double Now=FPlatformTime::Seconds();
        if (CompleteSpeechStartedAt==0) CompleteSpeechStartedAt=Now;
        if (Now-CompleteSpeechStartedAt>25) { Test->AddError(TEXT("完整逐句播放测试超时")); return End(); }
        if (CompleteSpeechStage==0)
        {
            Test->AddInfo(TEXT("使用本机合成的固定语音重复两句，检查完整播放和打断；本次不调用模型"));
            if (!Approve(A.Get(),EDeliveryNpcAction::LookAt,B.Get(),TEXT("先把话说清楚，再走也不迟"),true)) return End();
            CompleteSpeechStage=1;
            return false;
        }
        TArray<ADeliveryNpcCharacter*> Npcs=RemoteAs;Npcs.Insert(A.Get(),0);
        if (CompleteSpeechStage==3)
        {
            if (Now-CompleteSpeechCancelAt<.75) return false;
            for (auto* Npc:Npcs)
            {
                Test->TestFalse(TEXT("三端都停止被打断的声音"),Npc->Presentation->IsRecordedVoicePlaying());
                Test->TestFalse(TEXT("三端都停止被打断的字幕"),Npc->Presentation->GetDisplayedSpeech().bActive);
            }
            Test->TestEqual(TEXT("固定语音检查没有遗留模型请求"),Server->GetSubsystem<UDeliveryAiGatewaySubsystem>()->GetPendingCount(),0);
            return End();
        }
        bool bAllStopped=true;
        for (int32 Index=0;Index<Npcs.Num();++Index)
        {
            const auto* Presentation=Npcs[Index]->Presentation.Get();
            const auto Display=Presentation->GetDisplayedSpeech();
            bAllStopped&=!Display.bActive && !Presentation->IsRecordedVoicePlaying();
            if (!Display.bActive || !Presentation->IsRecordedVoicePlaying()) continue;
            const int32 Key=Index*10+Display.Sequence;
            if (!CompleteSpeechStarted.Contains(Key))
            {
                CompleteSpeechStarted.Add(Key);
                Test->AddInfo(FString::Printf(TEXT("CompleteSpeech round=%d view=%d sentence=%d started"),CompleteSpeechStage,Index,Display.Sequence));
            }
            auto& Progress=CompleteSpeechProgress.FindOrAdd(Key);
            Progress=FMath::Max(Progress,Presentation->GetDisplayedSpeechProgress());
        }
        if (CompleteSpeechStage==1)
        {
            if (CompleteSpeechStarted.Num()<6 || !bAllStopped) return false;
            for (int32 View=0;View<Npcs.Num();++View)
                for (int32 Sentence=0;Sentence<2;++Sentence)
                    Test->TestTrue(FString::Printf(TEXT("第%d端第%d句自然播放完整"),View,Sentence),CompleteSpeechProgress.FindRef(View*10+Sentence)>=.999f);
            Test->AddInfo(TEXT("主机与两个客户端都自然播放完两句话，继续检查主动打断"));
            CompleteSpeechStarted.Reset();CompleteSpeechProgress.Reset();
            if (!Approve(A.Get(),EDeliveryNpcAction::LookAt,B.Get(),TEXT("先把话说清楚，再走也不迟"),true)) return End();
            CompleteSpeechStage=2;
            return false;
        }
        if (CompleteSpeechStarted.Num()<Npcs.Num()) return false;
        for (int32 View=0;View<Npcs.Num();++View) if (CompleteSpeechProgress.FindRef(View*10)<.1f) return false;
        A->Agent->CancelPlan(TEXT("CompleteSpeechInterrupted"));
        CompleteSpeechCancelAt=Now;CompleteSpeechStage=3;
        return false;
    }
    bool CheckConversationPanel(UUserWidget* Panel)
    {
        bool Valid=Test->TestEqual(TEXT("使用正式交谈蓝图"),Panel->GetClass()->GetPathName(),FString(TEXT("/Game/UI/Conversation/WBP_Conversation.WBP_Conversation_C")));
        Valid=Test->TestNotNull(TEXT("蓝图中的输入框"),Cast<UEditableTextBox>(Panel->GetWidgetFromName(TEXT("Input")))) && Valid;
        Valid=Test->TestNotNull(TEXT("蓝图中的回复文本"),Cast<UTextBlock>(Panel->GetWidgetFromName(TEXT("Reply")))) && Valid;
        Valid=Test->TestNotNull(TEXT("蓝图中的说话人名称"),Cast<UTextBlock>(Panel->GetWidgetFromName(TEXT("SpeakerName")))) && Valid;
        for (const FName Name:{FName(TEXT("SendButton")),FName(TEXT("ApologizeButton")),FName(TEXT("ProvokeButton")),FName(TEXT("CloseButton"))})
            Valid=Test->TestNotNull(TEXT("蓝图中的交谈按钮"),Cast<UButton>(Panel->GetWidgetFromName(Name))) && Valid;
        return Valid;
    }
    bool ClickConversationButton(UUserWidget* Panel,FName Name)
    {
        auto* Button=Cast<UButton>(Panel->GetWidgetFromName(Name));
        if (!Test->TestNotNull(TEXT("找到实际蓝图按钮"),Button)) return false;
        const auto Bindings=Button->OnClicked.GetAllObjects();
        if (!Test->TestEqual(TEXT("按钮只绑定一次当前界面的处理函数"),Bindings.FilterByPredicate([Panel](const UObject* Object){return Object==Panel;}).Num(),1)) return false;
        Button->OnClicked.Broadcast();
        return true;
    }
    bool CheckReopenedConversation(APlayerController* PC,ADeliveryNpcCharacter* RemoteNpc)
    {
        auto* HUD=Cast<ADeliveryHUD>(PC->GetHUD());
        auto* Conversation=PC->FindComponentByClass<UDeliveryConversationComponent>();
        if (!Test->TestNotNull(TEXT("重新打开界面时 HUD 存在"),HUD) || !Conversation) return End();
        TArray<UUserWidget*> Panels;
        UWidgetBlueprintLibrary::GetAllWidgetsOfClass(PC,Panels,UDeliveryConversationWidget::StaticClass(),true);
        UUserWidget* Panel=nullptr;
        for (auto* Candidate:Panels) if (Candidate->GetOwningPlayer()==PC) { Panel=Candidate;break; }
        if (!Test->TestNotNull(TEXT("找到当前玩家的交谈界面"),Panel)) return End();
        if (UiReopenStage==0)
        {
            auto* OldButton=Cast<UButton>(Panel->GetWidgetFromName(TEXT("ApologizeButton")));
            PreviousPanel=Panel;
            if (!ClickConversationButton(Panel,TEXT("CloseButton"))) return End();
            Test->TestFalse(TEXT("结束按钮关闭交谈界面"),HUD->IsPanelOpen(TEXT("ResidentConversation")));
            if (OldButton) Test->TestEqual(TEXT("旧界面的按钮已解除绑定"),OldButton->OnClicked.GetAllObjects().Num(),0);
            HUD->OpenResidentConversation(RemoteNpc);
            UiReopenStage=1;
            UiReopenAt=FPlatformTime::Seconds();
            return false;
        }
        if (UiReopenStage==1)
        {
            if (FPlatformTime::Seconds()-UiReopenAt<.25) return false;
            if (!Test->TestTrue(TEXT("重新打开创建新的界面实例"),Panel!=PreviousPanel.Get()) || !CheckConversationPanel(Panel)) return End();
            for (const auto& Event:A->Agent->Evidence) if (Event.Kind==TEXT("PlayerSpeech")) UiPreviousMessages.Add(Event.EventId);
            const FGuid Before=Conversation->GetLastSubmissionId();
            if (!ClickConversationButton(Panel,TEXT("ApologizeButton"))) return End();
            UiNewMessage=Conversation->GetLastSubmissionId();
            if (!Test->TestTrue(TEXT("重建后点击按钮产生新的发言"),UiNewMessage.IsValid() && UiNewMessage!=Before)) return End();
            UiReopenStage=2; UiReopenAt=FPlatformTime::Seconds();
            return false;
        }
        if (FPlatformTime::Seconds()-UiReopenAt<.75) return false;
        const int32 NewMessages=A->Agent->Evidence.FilterByPredicate([this](const auto& Event)
        { return Event.Kind==TEXT("PlayerSpeech") && !UiPreviousMessages.Contains(Event.EventId); }).Num();
        Test->TestEqual(TEXT("重建后一次点击只提交一条玩家发言"),NewMessages,1);
        Test->TestTrue(TEXT("服务器收到重建界面发出的消息"),A->Agent->Evidence.ContainsByPredicate([this](const auto& Event){return Event.EventId==UiNewMessage;}));
        Test->TestEqual(TEXT("未发起额外模型调用"),A->GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()->GetPendingCount(),0);
        A->Agent->CancelPlan(TEXT("UiReopenTestEnd"));
        if (!ClickConversationButton(Panel,TEXT("CloseButton"))) return End();
        UiReopenStage=3;
        return false;
    }
    bool UpdateLiveConversation(UWorld* Server,const TArray<UWorld*>& Clients,const TArray<ADeliveryNpcCharacter*>& RemoteAs,APlayerController* PC)
    {
        const double Now=FPlatformTime::Seconds();
        if (FacePoseIndex!=INDEX_NONE) return UpdateFacePreview(Server,Clients);
        if (!bSavedLiveRequest && A->Agent->IsDecisionPending() && A->Agent->ActiveRequest.bPlayerRequest && A->Agent->ActiveRequest.RequestId.IsValid())
        {
            const FString Path=FPaths::ProjectSavedDir()/TEXT("AIResearch/live-request-context.json");
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
            bSavedLiveRequest=FFileHelper::SaveStringToFile(DeliveryNpc::EncodeRequest(A->Agent->ActiveRequest),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Test->TestTrue(TEXT("保存本次测试发给模型的信息"),bSavedLiveRequest);
        }
        if (Now-LiveSubmittedAt>35)
        {
            Test->AddError(FString::Printf(TEXT("真实模型交谈超时 round=%d speech=%s result=%s"),LiveRound,*LiveMessage.ToString(),
                *PC->FindComponentByClass<UDeliveryConversationComponent>()->GetLastResult().ToString()));
            return End();
        }
        TArray<ADeliveryNpcCharacter*> Npcs=RemoteAs; Npcs.Insert(A.Get(),0);
        if (LiveStopAt>0)
        {
            if (Now-LiveStopAt<.75) return false;
            if (!bLiveStopChecked)
            {
                for (auto* Npc:Npcs)
                {
                    Test->TestFalse(TEXT("打断后停止字幕"),Npc->Presentation->GetDisplayedSpeech().bActive);
                    Test->TestFalse(TEXT("打断后停止语音"),Npc->Presentation->IsRecordedVoicePlaying());
                }
                Test->TestFalse(TEXT("打断后结束模型请求"),A->Agent->IsDecisionPending());
                Test->TestFalse(TEXT("打断后移除待播语音"),A->Presentation->HasPendingSpeech());
                Test->TestEqual(TEXT("本次交谈请求已取消"),Server->GetSubsystem<UDeliveryAiGatewaySubsystem>()->GetPendingCount(),0);
                Test->AddInfo(TEXT("真实模型两轮交谈完成：道歉、拱火、三端播放与打断；台词和动作均由后端返回"));
                bLiveStopChecked=true;
            }
            if (UiReopenStage<3) return CheckReopenedConversation(PC,RemoteAs[0]);
            if (FParse::Param(FCommandLine::Get(),TEXT("DeliveryAIFacePreview")))
            {
                FacePoseIndex=0;
                Test->AddInfo(TEXT("开始表情预览：测试依次选择配置中的表情，画面用于检查形态和材质，不表示模型选择了这些情绪"));
                return false;
            }
            return End();
        }
        for (int32 Index=0;Index<Npcs.Num();++Index)
        {
            auto* Npc=Npcs[Index]; const auto Speech=Npc->Presentation->GetDisplayedSpeech();
            if (Npc->Agent->IsListening() && !LiveListening.Contains(Index))
            {
                LiveListening.Add(Index);
                Test->AddInfo(FString::Printf(TEXT("LiveModel round=%d view=%d stage=listening elapsed_ms=%.0f"),LiveRound,Index,(Now-LiveSubmittedAt)*1000));
            }
            if (Npc->Agent->IsListening() && Now-LiveSubmittedAt>=.35 && !LiveAnimationChecked.Contains(Index))
            {
                auto* Animation=Cast<UDeliveryNpcConversationAnimInstance>(Npc->GetMesh()->GetPostProcessInstance());
                if (Test->TestNotNull(TEXT("正式角色运行交谈动画蓝图"),Animation))
                {
                    Test->TestTrue(TEXT("倾听时姿态已开始过渡"),Animation->PostureWeight>0);
                    if (IsValid(Npc->Agent->GetListeningSpeaker())) Test->TestTrue(TEXT("倾听时视线已转向玩家"),Animation->LookWeight>0);
                }
                LiveAnimationChecked.Add(Index);
                if (!bCapturedListening) { CaptureViews(Server,Clients,TEXT("model-listening"));bCapturedListening=true; }
            }
            if (LiveRound==1 && LiveListening.Contains(Index) && Now-LiveSubmittedAt>.3)
                Test->TestFalse(TEXT("玩家插话后上一段对白已停止"),Speech.bActive && Speech.ReplyToEventId==PreviousMessage);
            if (Speech.bActive && Speech.ReplyToEventId==LiveMessage && Speech.bRecordedVoice && Npc->Presentation->IsRecordedVoicePlaying() && !LivePlayed.Contains(Index))
            {
                LivePlayed.Add(Index);
                Test->AddInfo(FString::Printf(TEXT("LiveModel round=%d view=%d stage=audio_play elapsed_ms=%.0f speech=%s text=%s"),
                    LiveRound,Index,(Now-LiveSubmittedAt)*1000,*LiveMessage.ToString(),*Speech.Text));
                Test->TestFalse(TEXT("模型返回了实际台词"),Speech.Text.TrimStartAndEnd().IsEmpty());
            }
        }
        if (LivePlayed.Num()!=Npcs.Num()) return false;
        Test->TestEqual(TEXT("三端均显示了倾听状态"),LiveListening.Num(),Npcs.Num());
        if (LiveRound==0)
        {
            if (Now-LiveSubmittedAt<1.2) return false;
            CaptureViews(Server,Clients,TEXT("model-apology"));
            TArray<UUserWidget*> Panels;
            UWidgetBlueprintLibrary::GetAllWidgetsOfClass(PC,Panels,UDeliveryConversationWidget::StaticClass(),true);
            for (auto* Panel:Panels)
            {
                if (Panel->GetOwningPlayer()!=PC) continue;
                if (Panel->GetWidgetFromName(TEXT("ProvokeButton")))
                {
                    PreviousMessage=LiveMessage;
                    if (!ClickConversationButton(Panel,TEXT("ProvokeButton"))) return End();
                    LiveMessage=PC->FindComponentByClass<UDeliveryConversationComponent>()->GetLastSubmissionId();
                    if (!Test->TestTrue(TEXT("拱火按钮发出另一条消息"),LiveMessage.IsValid() && LiveMessage!=PreviousMessage)) return End();
                    LiveRound=1; LiveSubmittedAt=Now; LiveListening.Reset(); LivePlayed.Reset();
                    Test->AddInfo(FString::Printf(TEXT("LiveModel submitted round=1 speech=%s button=Provoke"),*LiveMessage.ToString()));
                    return false;
                }
            }
            Test->AddError(TEXT("拱火时未找到玩家的交谈面板"));
            return End();
        }
        CaptureViews(Server,Clients,TEXT("model-provoke"));
        // 保存第二轮播放结果后打断交谈，检查三端字幕和声音都停止
        GetMutableDefault<UDeliveryAiSettings>()->bEnabled=false;
        B->Agent->CancelPlan(TEXT("LiveTestEnd"));
        A->Agent->CancelPlan(TEXT("LiveTestInterrupted"));
        LiveStopAt=Now;
        return false;
    }
    bool UpdateFacePreview(UWorld* Server,const TArray<UWorld*>& Clients)
    {
        static const FName Poses[]={TEXT("Calm"),TEXT("Angry"),TEXT("Surprised"),TEXT("Listening"),TEXT("Thinking")};
        if (FacePoseIndex>=UE_ARRAY_COUNT(Poses)) return End();
        const FName Pose=Poses[FacePoseIndex];
        TArray<UWorld*> Worlds=Clients; Worlds.Insert(Server,0);
        if (FaceCaptureAt==0)
        {
            for (auto* World:Worlds)
                for (TActorIterator<ADeliveryNpcCharacter> It(World);It;++It)
                {
                    auto* Npc=*It;
                    if (!OriginalFaceSettings.Contains(Npc)) OriginalFaceSettings.Add(Npc,Npc->Presentation->Expressions.Get());
                    auto* Original=OriginalFaceSettings.FindRef(Npc).Get();
                    if (!Test->TestNotNull(TEXT("正式角色配置了表情资产"),Original)) return End();
                    const auto* Selected=Original->Poses.Find(Pose);
                    if (!Test->TestNotNull(TEXT("找到本次预览的表情配置"),Selected)) return End();
                    auto* Preview=DuplicateObject<UDeliveryNpcExpressionSet>(Original,Npc->Presentation);
                    Preview->ClearFlags(RF_Public|RF_Standalone); Preview->SetFlags(RF_Transient);
                    for (auto& Pair:Preview->Poses) Pair.Value=*Selected;
                    // 近景固定在睁眼时拍摄，方便比较各个表情的形状
                    Preview->BlinkCurve=NewObject<UCurveFloat>(Preview);
                    Npc->Presentation->Expressions=Preview;
                }
            FaceCaptureAt=FPlatformTime::Seconds()+.8;
            return false;
        }
        if (FPlatformTime::Seconds()<FaceCaptureAt) return false;
        CaptureViews(Server,Clients,*FString::Printf(TEXT("face-%s"),*Pose.ToString()));
        for (int32 View=0;View<Worlds.Num();++View)
            for (TActorIterator<ADeliveryNpcCharacter> It(Worlds[View]);It;++It)
                CaptureFace(*It,Pose,View);
        ++FacePoseIndex; FaceCaptureAt=0;
        return false;
    }
    void CaptureFace(ADeliveryNpcCharacter* Npc,FName Pose,int32 View,float SideAngle=0)
    {
        auto* Mesh=Npc->GetMesh();
        const FBoxSphereBounds Bounds=Mesh->Bounds;
        const bool bHasHead=Mesh->DoesSocketExist(TEXT("Head"));
        const FVector HeadSocket=Mesh->GetSocketLocation(TEXT("Head"));
        // 按模型整体高度确定脸部中心，镜头保留额头和下巴周围的空间
        FVector Target=Bounds.Origin+FVector(0,0,Bounds.BoxExtent.Z*.45);
        if (bHasHead)
        {
            Target.X=HeadSocket.X;
            Target.Y=HeadSocket.Y;
        }
        const float Distance=FMath::Max(140.f,Bounds.BoxExtent.Z*2.f);
        const FVector Position=Target+(Npc->GetActorForwardVector()+Npc->GetActorRightVector()*SideAngle).GetSafeNormal()*Distance;
        auto* Capture=Npc->GetWorld()->SpawnActor<ASceneCapture2D>(Position,(Target-Position).Rotation());
        auto* Camera=Capture->GetCaptureComponent2D();
        // 近景使用临时正面补光，检查眼睛、眉毛和贴图细节
        auto* Fill=Npc->GetWorld()->SpawnActor<APointLight>(Position+FVector(0,0,30),FRotator::ZeroRotator);
        Fill->PointLightComponent->SetMobility(EComponentMobility::Movable);
        Fill->PointLightComponent->SetUseInverseSquaredFalloff(false);
        Fill->PointLightComponent->SetIntensityUnits(ELightUnits::Unitless);
        Fill->PointLightComponent->SetIntensity(3.f);
        Fill->PointLightComponent->SetAttenuationRadius(Distance*5.f);
        Fill->PointLightComponent->SetCastShadows(false);
        TStrongObjectPtr<UTextureRenderTarget2D> Texture(NewObject<UTextureRenderTarget2D>());
        Texture->ClearColor=FLinearColor(.025f,.035f,.05f,1.f);
        Texture->InitCustomFormat(768,768,PF_B8G8R8A8,false);
        Texture->UpdateResourceImmediate(true);
        Camera->TextureTarget=Texture.Get(); Camera->FOVAngle=50;
        Camera->bCaptureEveryFrame=false; Camera->bCaptureOnMovement=false;
        Camera->bAlwaysPersistRenderingState=true;
        Camera->CaptureSource=SCS_FinalColorLDR;
        Camera->PostProcessSettings.bOverride_AutoExposureMethod=true;
        Camera->PostProcessSettings.AutoExposureMethod=AEM_Manual;
        Camera->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure=true;
        Camera->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure=false;
        Camera->PostProcessSettings.bOverride_AutoExposureBias=true;
        Camera->PostProcessSettings.AutoExposureBias=0;
        Camera->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
        Camera->ShowOnlyComponent(Mesh);
        Test->AddInfo(FString::Printf(TEXT("FaceCamera npc=%s view=%d actor=%s mesh=%s scale=%s boundsCenter=%s boundsExtent=%s hasHead=%d head=%s target=%s camera=%s rotation=%s fov=50"),
            *GetNameSafe(Npc),View,*Npc->GetActorLocation().ToString(),*Mesh->GetComponentLocation().ToString(),*Mesh->GetComponentScale().ToString(),
            *Bounds.Origin.ToString(),*Bounds.BoxExtent.ToString(),bHasHead,*HeadSocket.ToString(),*Target.ToString(),*Position.ToString(),*(Target-Position).Rotation().ToString()));
        Camera->CaptureScene();
        TArray<FColor> Pixels;
        const bool bRead=Texture->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        Test->TestTrue(TEXT("读取人物面部近景"),bRead);
        if (bRead && !Pixels.IsEmpty())
        {
            int32 VisiblePixels=0;
            for (auto& Pixel:Pixels)
            {
                Pixel.A=255;
                if (Pixel.R>35 || Pixel.G>35 || Pixel.B>35) ++VisiblePixels;
            }
            Test->AddInfo(FString::Printf(TEXT("FacePreview bright_pixels=%.3f；截图构图、光照及表情效果需查看图片确认"),double(VisiblePixels)/Pixels.Num()));
            if (VisiblePixels<Pixels.Num()/100) Test->AddWarning(TEXT("面部截图大部分为黑色，请检查补光和镜头位置"));
            const FString Folder=FPaths::ProjectSavedDir()/(bPlayerGlance?TEXT("Automation/ResidentViews/PlayerGlance/Faces"):TEXT("Automation/ResidentViews/LiveModel/Faces"));
            IFileManager::Get().MakeDirectory(*Folder,true);
            const FString Name=Npc->GetClass()==A->GetClass()?TEXT("Loud"):TEXT("Shrewd");
            const FString Path=Folder/FString::Printf(TEXT("%s-%s-view%d.png"),*Name,*Pose.ToString(),View);
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(768,768,Pixels,Png);
            Test->TestTrue(TEXT("保存人物面部近景"),FFileHelper::SaveArrayToFile(Png,*Path));
            Test->AddInfo(FString::Printf(TEXT("FacePreview pose=%s npc=%s view=%d path=%s"),*Pose.ToString(),*Name,View,*Path));
        }
        Capture->Destroy(); Fill->Destroy();
    }
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
            const FString Folder=FPaths::ProjectSavedDir()/TEXT("Automation/ResidentViews")/(bPlayerGlance?TEXT("PlayerGlance"):((!LiveEndpoint.IsEmpty() || bFacePreviewOnly)?TEXT("LiveModel"):(bActualMap?(bHostDriver?TEXT("MapHost"):TEXT("MapClient")):TEXT("FlatThree"))));
            IFileManager::Get().MakeDirectory(*Folder,true);
            Test->TestTrue(TEXT("保存游戏视口证据"),FFileHelper::SaveArrayToFile(Png,*(Folder/FString::Printf(TEXT("%s-%s%d.png"),Phase,Index==0?TEXT("host"):TEXT("client"),Index))));
            // Scene 读回不包含 Slate/UMG，窗口截图另外保存交谈面板和字幕
            auto Window=Local->ViewportClient->GetWindow(); FIntVector WindowSize=FIntVector::ZeroValue;
            if (Window.IsValid() && FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(),Pixels,WindowSize))
            {
                FImageUtils::PNGCompressImageArray(WindowSize.X,WindowSize.Y,Pixels,Png);
                Test->TestTrue(TEXT("保存带 HUD 的窗口证据"),FFileHelper::SaveArrayToFile(Png,*(Folder/FString::Printf(TEXT("%s-%s%d-ui.png"),Phase,Index==0?TEXT("host"):TEXT("client"),Index))));
            }
        }
    }
    bool Approve(ADeliveryNpcCharacter* Resident,EDeliveryNpcAction Action,AActor* Target,const FString& Speech,bool bTwoSentences=false)
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
        if (Action==EDeliveryNpcAction::LookAt && !Speech.IsEmpty())
        {
            if (!Resident->Agent->ReceiveStreamDecision(P.RequestId,P)) return false;
            FDeliveryNpcSentence Sentence;Sentence.Sequence=0;Sentence.Text=Speech;Sentence.Emotion=P.Emotion;
            const FString Audio=FPaths::ProjectSavedDir()/TEXT("AIResearch/pie-speech.pcm");
            if (!Test->TestTrue(TEXT("加载本机 TTS 生成的语音样本"),FFileHelper::LoadFileToArray(Sentence.Pcm,*Audio))) return false;
            Resident->Agent->ReceiveSentence(P.RequestId,Sentence);
            if (bTwoSentences)
            {
                Sentence.Sequence=1;
                Resident->Agent->ReceiveSentence(P.RequestId,Sentence);
            }
            Resident->Agent->FinishReply(P.RequestId,true,bTwoSentences?2:1);
        }
        else Resident->Agent->ReceiveDecision(P.RequestId,&P,NAME_None);
        return Test->TestEqual(TEXT("建议通过正式审批"),Resident->Agent->GetPublicState().Status,EDeliveryNpcActionStatus::Accepted);
    }
    bool End() { RestoreFaceSettings(); GEditor->RequestEndPlayMap(); bEnding=true; return false; }
    FAutomationTestBase* Test;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TWeakObjectPtr<ADeliveryNpcCharacter> A,B;
    TWeakObjectPtr<ADeliveryMotorbike> Bike;
    TWeakObjectPtr<APlayerController> DriverPC;
    double StartedAt=0;
    float ImpactStartedAt=-1;
    double ImpactRealStartedAt=0;
    uint64 ImpactFrame=0;
    float NextTime=0;
    int32 Stage=0;
    bool bStarted=false,bEnding=false,bOldEnabled=false;
    bool bOldStreaming=false,bChangedSettings=false;
    FString LiveEndpoint,OldEndpoint;
    float OldTimeout=0;
    double LiveSubmittedAt=0,LiveStopAt=0;
    int32 LiveRound=0;
    FGuid LiveMessage,PreviousMessage;
    TSet<int32> LiveListening,LivePlayed,LiveAnimationChecked;
    bool bCapturedListening=false;
    bool bFacePreviewOnly=false;
    bool bCompleteSpeech=false;
    bool bPlayerGlance=false;
    int32 GlanceStage=0,GlanceRound=0,GlancePlayerId=0;
    float GlanceStartedAt=0;
    TArray<FQuat> GlanceHeadBefore;
    TSet<int32> GlanceTargets,GlanceHeads,GlanceEyes;
    int32 CompleteSpeechStage=0;
    double CompleteSpeechStartedAt=0,CompleteSpeechCancelAt=0;
    TMap<int32,float> CompleteSpeechProgress;
    TSet<int32> CompleteSpeechStarted;
    bool bSavedLiveRequest=false;
    bool bLiveStopChecked=false;
    int32 UiReopenStage=0;
    double UiReopenAt=0;
    FGuid UiNewMessage;
    TSet<FGuid> UiPreviousMessages;
    TWeakObjectPtr<UUserWidget> PreviousPanel;
    int32 FacePoseIndex=INDEX_NONE;
    double FaceCaptureAt=0;
    TMap<TWeakObjectPtr<ADeliveryNpcCharacter>,TWeakObjectPtr<UDeliveryNpcExpressionSet>> OriginalFaceSettings;
    bool bActualMap=false,bHostDriver=false;
    bool bRetaliationStarted=false,bRetaliationChecked=false;
    bool bConversationSent=false;
    FVector Origin=FVector::ZeroVector;
    FVector InitialA=FVector::ZeroVector;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPlayerGlancePIETest,"Delivery.AI.PlayerGlanceMultiplayer",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryPlayerGlancePIETest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(),TEXT("DeliveryAIPlayerGlance")))
    { AddWarning(TEXT("未运行多人注视检查：需要显式提供 -DeliveryAIPlayerGlance")); return true; }
    if (!FApp::CanEverRender()) { AddError(TEXT("多人注视检查需要真实渲染 PIE")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryResidentPIECommand(this,false,false,FString(),false,false,true));
    return true;
}
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryResidentLiveModelTest,"Delivery.AI.ResidentLiveModel",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryResidentLiveModelTest::RunTest(const FString&)
{
    FString Endpoint;
    if (!FParse::Value(FCommandLine::Get(),TEXT("DeliveryAILiveEndpoint="),Endpoint))
    {
        AddWarning(TEXT("未运行真实模型测试：需要显式提供 -DeliveryAILiveEndpoint=http://127.0.0.1:端口/v1/decisions"));
        return true;
    }
    if (!FApp::CanEverRender()) { AddError(TEXT("真实模型交谈检查需要渲染和音频")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryResidentPIECommand(this,false,false,Endpoint));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryResidentFacePreviewTest,"Delivery.AI.ResidentFacePreview",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryResidentFacePreviewTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(),TEXT("DeliveryAIFacePreviewOnly")))
    {
        AddWarning(TEXT("未运行表情预览：需要显式提供 -DeliveryAIFacePreviewOnly"));
        return true;
    }
    if (!FApp::CanEverRender()) { AddError(TEXT("表情预览需要渲染")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryResidentPIECommand(this,false,false,FString(),true));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryResidentCompleteSpeechTest,"Delivery.AI.ResidentCompleteSpeech",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryResidentCompleteSpeechTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(),TEXT("DeliveryAICompleteSpeech")))
    {
        AddWarning(TEXT("未运行完整语音检查：需要显式提供 -DeliveryAICompleteSpeech")); return true;
    }
    if (!FApp::CanEverRender()) { AddError(TEXT("完整语音检查需要渲染和音频"));return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryResidentPIECommand(this,false,false,FString(),false,true));
    return true;
}
#endif
