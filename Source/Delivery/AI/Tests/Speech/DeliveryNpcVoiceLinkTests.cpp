#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundAttenuation.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcVoiceLinkTest,"Delivery.AI.VoiceLink",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcVoiceLinkTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("World"),World)) { Settings->bEnabled=bEnabled; return false; }
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=bEnabled;
    };
    FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Companion=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(10000,0,90),FRotator::ZeroRotator,Spawn);
    auto* Neighbor=World->SpawnActor<ADeliveryNpcCharacter>(FVector(10300,0,90),FRotator::ZeroRotator,Spawn);
    auto* SecondCompanion=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(13000,0,90),FRotator::ZeroRotator,Spawn);
    auto* Owner=World->SpawnActor<APlayerController>();
    auto* Other=World->SpawnActor<APlayerController>();
    if (!Companion || !Neighbor || !SecondCompanion || !Owner || !Other) return false;
    Owner->PlayerState=World->SpawnActor<APlayerState>();
    Other->PlayerState=World->SpawnActor<APlayerState>();
    auto* PlayerCharacter=World->SpawnActor<ADeliveryCharacter>(FVector::ZeroVector,FRotator::ZeroRotator,Spawn);
    auto* Bike=World->SpawnActor<ADeliveryMotorbike>(FVector(100,0,90),FRotator::ZeroRotator,Spawn);
    if (!PlayerCharacter || !Bike) return false;
    Owner->Possess(PlayerCharacter);
    Other->Possess(World->SpawnActor<APawn>(FVector(200,0,90),FRotator::ZeroRotator,Spawn));
    auto* Conversation=NewObject<UDeliveryConversationComponent>(Owner);
    Owner->AddInstanceComponent(Conversation); Conversation->RegisterComponent();
    const auto* OwnerProperty=FindFProperty<FObjectPropertyBase>(UDeliveryCompanionComponent::StaticClass(),TEXT("OwnerPlayerState"));
    if (!TestNotNull(TEXT("同伴归属字段"),OwnerProperty)) return false;
    const auto* EncounterProperty=FindFProperty<FStructProperty>(ADeliveryCompanionCharacter::StaticClass(),TEXT("Encounter"));
    if (!TestNotNull(TEXT("同伴结识阶段字段"),EncounterProperty)) return false;
    // 构造复制后的已结识状态，不把归属和阶段设置当作实际招募验收
    OwnerProperty->SetObjectPropertyValue_InContainer(Companion->Companion,Owner->PlayerState);
    OwnerProperty->SetObjectPropertyValue_InContainer(SecondCompanion->Companion,Owner->PlayerState);
    EncounterProperty->ContainerPtrToValuePtr<FDeliveryCompanionEncounterState>(Companion)->Phase=EDeliveryCompanionEncounterPhase::Joined;
    EncounterProperty->ContainerPtrToValuePtr<FDeliveryCompanionEncounterState>(SecondCompanion)->Phase=EDeliveryCompanionEncounterPhase::Joined;
    auto* View=Companion->Presentation.Get();
    TestNull(TEXT("普通消息不因同伴归属就获得连麦播放"),View->ResolveRadioListener(FGuid::NewGuid()));
    const FDeliverySpeechTurnToken LocalToken{FGuid::NewGuid(),1};
    Conversation->HandleVoiceStarted(LocalToken,Companion);
    const auto Handle=Conversation->LocalVoiceHandle;
    if (!TestTrue(TEXT("真实ServerBeginVoice签发远处同伴轮次"),Handle.IsValid())) return false;
    TestTrue(TEXT("授权消息确属这名同伴"),Conversation->IsVoiceReplyMessage(Handle.Token,Companion));
    TestNull(TEXT("同主人另一同伴不能借用此消息"),SecondCompanion->Presentation->ResolveRadioListener(Handle.Token));
    TestEqual(TEXT("服务器解析本句主人"),View->ResolveRadioListener(Handle.Token),Owner->PlayerState.Get());
    TestNull(TEXT("普通NPC不能借用连麦消息"),Neighbor->Presentation->ResolveRadioListener(Handle.Token));
    Conversation->HandleVoiceReleased(LocalToken);

    // 只构造已完成的控制关系，不把反射设置驾驶员当作真实上车操作验收
    const auto* DriverProperty=FindFProperty<FObjectPropertyBase>(ADeliveryMotorbike::StaticClass(),TEXT("Driver"));
    if (!TestNotNull(TEXT("驾驶员复制字段"),DriverProperty)) return false;
    auto* Turns=World->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    if (!TestNotNull(TEXT("服务器交谈轮次"),Turns) ||
        !TestTrue(TEXT("等待回复使用同一语音轮次"),Turns->CommitFinal(Owner,Companion,Handle,Handle.Token))) return false;
    TestTrue(TEXT("远处主人开始连麦倾听"),Companion->Agent->BeginListening(PlayerCharacter,Handle.Token));
    static_cast<UActorComponent*>(Companion->Agent.Get())->TickComponent(.01f,LEVELTICK_All,nullptr);
    TestTrue(TEXT("连麦不被现场900cm距离取消"),Companion->Agent->IsListening());
    DriverProperty->SetObjectPropertyValue_InContainer(Bike,PlayerCharacter);
    PlayerCharacter->AttachToActor(Bike,FAttachmentTransformRules::KeepWorldTransform);
    Owner->Possess(Bike);
    static_cast<UActorComponent*>(Companion->Agent.Get())->TickComponent(.01f,LEVELTICK_All,nullptr);
    TestTrue(TEXT("上车保持原回复归属"),Conversation->IsVoiceReplyMessage(Handle.Token,Companion));
    TestEqual(TEXT("车上倾听跟随当前控制对象"),Companion->Agent->GetListeningSpeaker(),static_cast<AActor*>(Bike));
    DriverProperty->SetObjectPropertyValue_InContainer(Bike,nullptr);
    PlayerCharacter->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    Owner->Possess(PlayerCharacter);
    static_cast<UActorComponent*>(Companion->Agent.Get())->TickComponent(.01f,LEVELTICK_All,nullptr);
    TestTrue(TEXT("下车保持原回复归属"),Conversation->IsVoiceReplyMessage(Handle.Token,Companion));
    TestEqual(TEXT("下车倾听回到原人物"),Companion->Agent->GetListeningSpeaker(),static_cast<AActor*>(PlayerCharacter));
    Companion->Agent->EndListening();

    FDeliveryNpcCompleteReply Reply;
    Reply.ReplyId=FGuid::NewGuid(); Reply.MessageId=Handle.Token;
    Reply.Text=TEXT("我还在送货，听见了。"); Reply.Listener=Neighbor;
    TestTrue(TEXT("完整回复进入正式开始入口"),View->StartCompleteReply(Reply));
    TestEqual(TEXT("连麦归属来自授权消息而不是SpeechTarget"),View->Speech.RadioListener.Get(),Owner->PlayerState.Get());
    TestTrue(TEXT("指定主人可使用本句连麦"),View->IsRadioListenerForPlayer(Owner,View->Speech.RadioListener));
    TestFalse(TEXT("另一玩家不能使用本句连麦"),View->IsRadioListenerForPlayer(Other,View->Speech.RadioListener));

    // 音频已合成的缓存前提，不运行模型、声卡或编码基准
    const FGuid Turn=View->Speech.TurnId;
    auto& Cached=View->ServerVoices.Add(Turn);
    Cached.RadioListener=View->Speech.RadioListener; Cached.Encoded={1,2,3};
    TestTrue(TEXT("主人可补取远处同伴这句缓存"),View->CanPlayerReceiveLinkedAudio(Owner,Turn));
    TestFalse(TEXT("其他玩家不能获得距离豁免"),View->CanPlayerReceiveLinkedAudio(Other,Turn));
    TestFalse(TEXT("未知句子不能借用当前连麦"),View->CanPlayerReceiveLinkedAudio(Owner,FGuid::NewGuid()));
    const FGuid OrdinaryTurn=FGuid::NewGuid();
    View->ServerVoices.Add(OrdinaryTurn).Encoded={1};
    TestFalse(TEXT("同一同伴的普通句子仍受距离限制"),View->CanPlayerReceiveLinkedAudio(Owner,OrdinaryTurn));
    View->ServerVoices.FindChecked(Turn).ExpiresAt=FPlatformTime::Seconds()-1;
    TestFalse(TEXT("过期缓存不能继续连麦补取"),View->CanPlayerReceiveLinkedAudio(Owner,Turn));
    View->ServerVoices.FindChecked(Turn).ExpiresAt=0;

    View->RecordedVoice=NewObject<UAudioComponent>(Companion);
    View->VoiceSet=NewObject<UDeliveryNpcVoiceSet>(View);
    View->VoiceSet->DialogueVolume=.6f;
    View->VoiceSet->DialogueAttenuation=NewObject<USoundAttenuation>(View);
    auto* OriginalAttenuation=View->VoiceSet->DialogueAttenuation.Get();
    View->ConfigureRecordedVoice(View->IsRadioListenerForPlayer(Owner,View->Speech.RadioListener));
    TestFalse(TEXT("主人这句关闭空间化"),View->RecordedVoice->bAllowSpatialization);
    TestTrue(TEXT("连麦使用独立衰减覆盖"),View->RecordedVoice->bOverrideAttenuation);
    TestFalse(TEXT("连麦不按距离衰减"),View->RecordedVoice->AttenuationOverrides.bAttenuate);
    TestEqual(TEXT("保留原人物音量"),View->RecordedVoice->VolumeMultiplier,.6f);
    View->ConfigureRecordedVoice(View->IsRadioListenerForPlayer(Other,View->Speech.RadioListener));
    TestTrue(TEXT("其他玩家恢复空间化"),View->RecordedVoice->bAllowSpatialization);
    TestEqual(TEXT("下一普通句恢复原衰减资产"),View->RecordedVoice->AttenuationSettings.Get(),OriginalAttenuation);
    TestFalse(TEXT("恢复资产后不沿用连麦覆盖"),View->RecordedVoice->bOverrideAttenuation);

    // 连麦旁听只保存事实，普通对白继续允许邻居接话
    const FGuid RadioFact=FGuid::NewGuid();
    View->Speech.TurnId=RadioFact; View->SpokenReply=TEXT("我还在送货，听见了。");
    View->ReplyMessage=Handle.Token; View->ReplyListener=Owner->GetPawn(); View->RecordSpokenReply(true);
    FDeliveryNpcDecisionRequest Request;
    TestTrue(TEXT("可读取邻居的实际旁听上下文"),Neighbor->Agent->BuildDecisionRequest(FGuid::NewGuid(),Request));
    const auto* Fact=Request.Evidence.FindByPredicate([&](const auto& Item){return Item.EventId==RadioFact;});
    if (TestNotNull(TEXT("连麦台词仍可作为附近旁听事实"),Fact)) TestFalse(TEXT("连麦旁听不触发新回复"),Fact->bNeedsReply);
    const FGuid OrdinaryFact=FGuid::NewGuid();
    View->Speech.TurnId=OrdinaryFact; View->Speech.RadioListener=nullptr; View->SpokenReply=TEXT("这附近谁在送货？");
    View->RecordSpokenReply(true);
    TestTrue(TEXT("普通对白仍可建立上下文"),Neighbor->Agent->BuildDecisionRequest(FGuid::NewGuid(),Request));
    Fact=Request.Evidence.FindByPredicate([&](const auto& Item){return Item.EventId==OrdinaryFact;});
    if (TestNotNull(TEXT("普通对白事实保留"),Fact)) TestTrue(TEXT("普通对白保留原接话规则"),Fact->bNeedsReply);

    OwnerProperty->SetObjectPropertyValue_InContainer(Companion->Companion,Other->PlayerState);
    TestFalse(TEXT("旧主人失去缓存豁免"),View->CanPlayerReceiveLinkedAudio(Owner,Turn));
    TestFalse(TEXT("新主人不能听旧主人的缓存"),View->CanPlayerReceiveLinkedAudio(Other,Turn));
    TestNull(TEXT("旧语音消息不能授权新主人"),View->ResolveRadioListener(Handle.Token));
    View->LocalVoices.Reset();
    auto& Local=View->LocalVoices.AddDefaulted_GetRef();
    Local.State.TurnId=Turn; Local.State.RadioListener=Owner->PlayerState;
    View->DisplayedSpeech=Local.State; View->DisplayedSpeech.bActive=true; View->bPlayingRadioVoice=true;
    View->PlayRecordedVoice();
    TestTrue(TEXT("归属变化后停止本机已经开始的旧连麦"),View->LocalVoices.IsEmpty() && !View->bPlayingRadioVoice);
    OwnerProperty->SetObjectPropertyValue_InContainer(Companion->Companion,Owner->PlayerState);
    TestTrue(TEXT("销毁前测试缓存仍有效"),View->CanPlayerReceiveLinkedAudio(Owner,Turn));
    Owner->PlayerState->Destroy();
    TestFalse(TEXT("收听者销毁后缓存失效"),View->CanPlayerReceiveLinkedAudio(Owner,Turn));
    return true;
}
#endif
