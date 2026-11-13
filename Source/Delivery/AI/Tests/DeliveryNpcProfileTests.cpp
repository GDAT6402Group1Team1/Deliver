#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Characters/DeliveryNpcProfile.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/DeliveryNpcVoiceCodec.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundAttenuation.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcProfileTest,"Delivery.AI.NpcProfiles",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcProfileTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();const bool Enabled=Settings->bEnabled;Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit);GEngine->DestroyWorldContext(World);World->DestroyWorld(false);Settings->bEnabled=Enabled; };
    auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>();auto* P=Npc->Presentation.Get();
    const FText OriginalName=Npc->DisplayName;const FString OriginalPersona=Npc->Agent->Persona;
    TestFalse(TEXT("没有选择人物表时保留原配置"),Npc->ApplyProfile());
    TestEqual(TEXT("未配表不改变原姓名"),Npc->DisplayName.ToString(),OriginalName.ToString());
    FDeliveryNpcDecisionRequest ProfileRequest;
    const FGuid ProfileWorldEpoch=FGuid::NewGuid();
    if (!TestTrue(TEXT("未配表的 NPC 仍能准备请求"),Npc->Agent->BuildDecisionRequest(ProfileWorldEpoch,ProfileRequest))) return false;
    TestTrue(TEXT("未配表时不发送虚构的人物编号"),ProfileRequest.ProfileId.IsEmpty());
    TestFalse(TEXT("没有声音素材时不添加短音"),P->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer,FGuid::NewGuid()));
    auto* Table=NewObject<UDataTable>();Table->RowStruct=FDeliveryNpcProfileRow::StaticStruct();
    auto* VoiceSet=NewObject<UDeliveryNpcVoiceSet>();
    auto* Heard=NewObject<USoundWave>();Heard->Duration=.4f;
    auto* Hit=NewObject<USoundWave>();Hit->Duration=.3f;
    VoiceSet->HeardPlayer=Heard;VoiceSet->Hit=Hit;
    FDeliveryNpcProfileRow Row;Row.DisplayName=FText::FromString(TEXT("测试人物"));Row.Persona=TEXT("爱开玩笑的邻居");
    Row.TtsVoice=TEXT("female");Row.VoiceSet=VoiceSet;
    Table->AddRow(TEXT("test"),Row);Npc->Profile.DataTable=Table;Npc->Profile.RowName=TEXT("missing");
    TestFalse(TEXT("缺少人物行时不改已有配置"),Npc->ApplyProfile());
    TestEqual(TEXT("无效行不改变性格"),Npc->Agent->Persona,OriginalPersona);
    Npc->Profile.RowName=TEXT("test");
    TestTrue(TEXT("人物行应用到现有组件"),Npc->ApplyProfile());
    TestEqual(TEXT("姓名来自人物表"),Npc->DisplayName.ToString(),Row.DisplayName.ToString());
    TestEqual(TEXT("性格来自人物表"),Npc->Agent->Persona,Row.Persona);
    if (!TestTrue(TEXT("使用人物表后准备请求"),Npc->Agent->BuildDecisionRequest(ProfileWorldEpoch,ProfileRequest))) return false;
    TestEqual(TEXT("背景资料使用人物表行名"),ProfileRequest.ProfileId,FString(TEXT("test")));
    Npc->DisplayName=FText::FromString(TEXT("换了显示名字"));
    if (!TestTrue(TEXT("改显示名字后仍可准备请求"),Npc->Agent->BuildDecisionRequest(ProfileWorldEpoch,ProfileRequest))) return false;
    TestEqual(TEXT("改显示名字不会改变背景资料编号"),ProfileRequest.ProfileId,FString(TEXT("test")));
    Npc->DisplayName=Row.DisplayName;
    Npc->Profile.RowName=NAME_None;
    if (!TestTrue(TEXT("清除人物行后重新准备请求"),Npc->Agent->BuildDecisionRequest(ProfileWorldEpoch,ProfileRequest))) return false;
    TestTrue(TEXT("重新使用请求对象不会保留旧人物编号"),ProfileRequest.ProfileId.IsEmpty());
    Npc->Profile.RowName=TEXT("test");
    TestEqual(TEXT("TTS 音色与短反应音分别配置"),P->TtsVoice,Row.TtsVoice);
    TestTrue(TEXT("人物表引用短反应音资产"),P->VoiceSet==VoiceSet);
    Row.TtsVoice=TEXT("unsupported-voice");Row.DisplayName=FText::FromString(TEXT("不应应用的姓名"));Table->AddRow(TEXT("test"),Row);
    TestFalse(TEXT("不支持的 TTS 音色拒绝整行配置"),Npc->ApplyProfile());
    TestEqual(TEXT("拒绝音色时姓名也保持原值"),Npc->DisplayName.ToString(),FString(TEXT("测试人物")));
    TestEqual(TEXT("拒绝音色时保留原 TTS 配置"),P->TtsVoice,FName(TEXT("female")));
    Row.TtsVoice=NAME_None;Row.DisplayName=FText::FromString(TEXT("测试人物"));Table->AddRow(TEXT("test"),Row);
    TestTrue(TEXT("TTS 留空可以保留原音色"),Npc->ApplyProfile());
    TestEqual(TEXT("空 TTS 配置不改变已有音色"),P->TtsVoice,FName(TEXT("female")));
    auto* Player=World->SpawnActor<AActor>();const FGuid HeardEvent=FGuid::NewGuid();
    Npc->Agent->BeginListening(Player,HeardEvent);
    TestEqual(TEXT("最后一次取消旧交谈后才播放倾听短音"),P->ReactionEventId,HeardEvent);
    if (!TestNotNull(TEXT("短音使用 UE 音频组件"),P->ReactionAudio.Get())) return false;
    TestTrue(TEXT("倾听使用配置的短音"),P->ReactionAudio->Sound==Heard);
    TestFalse(TEXT("相同事件不重复播放"),P->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer,HeardEvent));
    TestFalse(TEXT("未配置的反应类别保持安静"),P->PlayReaction(EDeliveryNpcVoiceReaction::Laugh,FGuid::NewGuid()));
    auto* ReactionAttenuation=NewObject<USoundAttenuation>();
    ReactionAttenuation->Attenuation.bAttenuate=true;
    ReactionAttenuation->Attenuation.bSpatialize=true;
    ReactionAttenuation->Attenuation.DistanceAlgorithm=EAttenuationDistanceModel::Linear;
    ReactionAttenuation->Attenuation.AttenuationShapeExtents=FVector(600,0,0);
    ReactionAttenuation->Attenuation.FalloffDistance=1200;
    VoiceSet->ReactionAttenuation=ReactionAttenuation;
    TestTrue(TEXT("人物配置可以更新下一声短音的范围"),P->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer,FGuid::NewGuid()));
    TestTrue(TEXT("短音使用人物指定的衰减资产"),P->ReactionAudio->AttenuationSettings==ReactionAttenuation && !P->ReactionAudio->bOverrideAttenuation);
    TestTrue(TEXT("交谈边缘加第三人称镜头仍能听见短音"),P->ReactionAudio->GetAttenuationSettingsToApply()->Evaluate(FTransform::Identity,FVector(1000,0,0))>.6f);
    TestNull(TEXT("短音配置不改变完整台词的衰减"),VoiceSet->DialogueAttenuation.Get());
    VoiceSet->ReactionAttenuation=nullptr;
    P->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer,FGuid::NewGuid());
    TestTrue(TEXT("清空配置后恢复原短音范围"),P->ReactionAudio->bOverrideAttenuation &&
        P->ReactionAudio->AttenuationSettings==nullptr && P->ReactionAudio->AttenuationOverrides.AttenuationShapeExtents.X==150);
    P->CancelSpeech();
    P->MulticastPlayReaction_Implementation(EDeliveryNpcVoiceReaction::HeardPlayer,HeardEvent,P->ServerTime());
    TestFalse(TEXT("取消后的迟到播放通知不会恢复声音"),P->ReactionEventId.IsValid());
    const FGuid StoppedBeforeStart=FGuid::NewGuid();
    P->MulticastStopReaction_Implementation(StoppedBeforeStart);
    P->MulticastPlayReaction_Implementation(EDeliveryNpcVoiceReaction::HeardPlayer,StoppedBeforeStart,P->ServerTime());
    TestFalse(TEXT("停止通知先到时仍能拒绝旧播放"),P->ReactionEventId.IsValid());
    const FGuid Current=FGuid::NewGuid();P->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer,Current);
    P->MulticastStopReaction_Implementation(HeardEvent);
    TestEqual(TEXT("旧事件的停止通知不影响新短音"),P->ReactionEventId,Current);
    FDeliveryNpcSpeechState Speech;Speech.ActionId=FGuid::NewGuid();Speech.TurnId=FGuid::NewGuid();Speech.bActive=true;
    Speech.Text=TEXT("正式台词");Speech.StartTime=P->ServerTime();Speech.Duration=1;
    P->AcceptLocalSpeech(Speech,false);P->PlayRecordedVoice();
    TestFalse(TEXT("正式字幕开始时停止倾听短音"),P->ReactionEventId.IsValid());
    TestTrue(TEXT("正式台词已经开始显示"),P->GetDisplayedSpeech().bActive);
    P->CancelSpeech();
    // 用短 PCM 进入实际播放入口，检查人物配置切换后音频组件使用哪份设置
    auto StartRecordedVoice=[P](UDeliveryNpcVoiceSet* Config)
    {
        P->CancelSpeech();P->VoiceSet=Config;
        FDeliveryNpcSpeechState State;State.ActionId=FGuid::NewGuid();State.TurnId=FGuid::NewGuid();
        State.bActive=true;State.bRecordedVoice=true;State.StartTime=P->ServerTime()-1;State.Duration=.24f;
        State.Text=TEXT("声音配置检查");State.SampleRate=16000;State.PcmBytes=6400;State.EncodedBytes=64;
        State.AudioFormatVersion=DeliveryNpcVoice::FormatVersion;
        P->StartStreamSpeech_Implementation(State);
        if (P->LocalVoices.IsEmpty()) return false;
        P->LocalVoices[0].Pcm.SetNumZeroed(P->LocalVoices[0].TotalBytes);
        P->PlayRecordedVoice();
        return P->RecordedVoice!=nullptr && P->RecordedWave!=nullptr;
    };
    auto* DialogueAttenuation=NewObject<USoundAttenuation>();
    DialogueAttenuation->Attenuation.bAttenuate=true;DialogueAttenuation->Attenuation.bSpatialize=true;
    DialogueAttenuation->Attenuation.AttenuationShapeExtents=FVector(720,0,0);
    DialogueAttenuation->Attenuation.FalloffDistance=1300;
    VoiceSet->DialogueVolume=1.4f;VoiceSet->DialogueAttenuation=DialogueAttenuation;
    const float ReactionVolume=VoiceSet->Volume,ReactionPitch=VoiceSet->Pitch,ReactionDuration=VoiceSet->MaxDuration;
    if (!TestTrue(TEXT("完整语音进入实际播放入口"),StartRecordedVoice(VoiceSet))) return false;
    auto* ReusedAudio=P->RecordedVoice.Get();
    TestEqual(TEXT("完整台词使用独立音量"),ReusedAudio->VolumeMultiplier,1.4f);
    TestTrue(TEXT("完整台词使用人物所选衰减资产"),ReusedAudio->AttenuationSettings==DialogueAttenuation &&
        !ReusedAudio->bOverrideAttenuation && ReusedAudio->GetAttenuationSettingsToApply()==&DialogueAttenuation->Attenuation);
    TestTrue(TEXT("完整台词保留空间声音"),ReusedAudio->bAllowSpatialization && DialogueAttenuation->Attenuation.bSpatialize);
    if (!TestTrue(TEXT("下一句可以不使用人物声音配置"),StartRecordedVoice(nullptr))) return false;
    TestTrue(TEXT("切换配置时复用原音频组件"),P->RecordedVoice==ReusedAudio);
    TestEqual(TEXT("未配人物声音资产时使用原音量"),ReusedAudio->VolumeMultiplier,1.f);
    TestTrue(TEXT("清空人物资产后不残留上一句衰减设置"),!ReusedAudio->AttenuationSettings && ReusedAudio->bOverrideAttenuation);
    const auto* DefaultAttenuation=ReusedAudio->GetAttenuationSettingsToApply();
    if (!TestNotNull(TEXT("缺省衰减有效"),DefaultAttenuation)) return false;
    TestTrue(TEXT("缺省声音仍按空间距离变小"),DefaultAttenuation->bAttenuate && DefaultAttenuation->bSpatialize);
    TestEqual(TEXT("缺省衰减使用线性曲线"),DefaultAttenuation->DistanceAlgorithm,EAttenuationDistanceModel::Linear);
    TestTrue(TEXT("交谈六米内保持原音量"),DefaultAttenuation->Evaluate(FTransform::Identity,FVector(600,0,0))>.99f);
    // 玩家距 NPC 六米、镜头再退四米时，旧的 150+800 范围已经听不见
    TestTrue(TEXT("六米交谈加四米镜头后仍保留至少六成音量"),DefaultAttenuation->Evaluate(FTransform::Identity,FVector(1000,0,0))>.6f);
    TestTrue(TEXT("远处仍会衰减至静音"),DefaultAttenuation->Evaluate(FTransform::Identity,FVector(1800,0,0))<.01f);
    VoiceSet->DialogueAttenuation=nullptr;VoiceSet->DialogueVolume=.55f;
    if (!TestTrue(TEXT("人物只配音量也能开始下一句"),StartRecordedVoice(VoiceSet))) return false;
    TestEqual(TEXT("缺少衰减资产时保留人物音量"),ReusedAudio->VolumeMultiplier,.55f);
    TestTrue(TEXT("人物未配衰减时同样使用默认距离"),ReusedAudio->bOverrideAttenuation &&
        ReusedAudio->GetAttenuationSettingsToApply()->Evaluate(FTransform::Identity,FVector(1000,0,0))>.6f);
    const float InvalidVolumes[]={-1.f,4.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()};
    const float ExpectedVolumes[]={0.f,2.f,1.f,1.f};
    for (int32 Index=0;Index<UE_ARRAY_COUNT(InvalidVolumes);++Index)
    {
        VoiceSet->DialogueVolume=InvalidVolumes[Index];
        if (!TestTrue(TEXT("非法音量不会阻止下一句播放"),StartRecordedVoice(VoiceSet))) return false;
        TestEqual(FString::Printf(TEXT("非法音量 %d 转为有效播放音量"),Index),ReusedAudio->VolumeMultiplier,ExpectedVolumes[Index]);
    }
    TestEqual(TEXT("完整台词不修改短音音量"),VoiceSet->Volume,ReactionVolume);
    TestEqual(TEXT("完整台词不修改短音音调"),VoiceSet->Pitch,ReactionPitch);
    TestEqual(TEXT("完整台词不修改短音时长"),VoiceSet->MaxDuration,ReactionDuration);
    VoiceSet->DialogueVolume=1.f;P->CancelSpeech();
    P->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer,FGuid::NewGuid());
    Npc->ReceiveImpact(FVector(100,0,0),Npc->GetActorLocation());
    TestTrue(TEXT("有效受击切换到受击短音"),P->ReactionEventId.IsValid() && P->ReactionAudio->Sound==Hit);
    P->CancelSpeech();
    P->MulticastPlayReaction_Implementation(EDeliveryNpcVoiceReaction::Hit,FGuid::NewGuid(),P->ServerTime()-4);
    TestFalse(TEXT("过期的短音通知不播放"),P->ReactionEventId.IsValid());
    for (int32 Index=0;Index<40;++Index) P->MulticastStopReaction_Implementation(FGuid::NewGuid());
    TestEqual(TEXT("取消事件记录有数量上限"),P->CancelledReactionEvents.Num(),32);
    P->EndPlay(EEndPlayReason::EndPlayInEditor);
    TestNull(TEXT("结束关卡后释放短音组件"),P->ReactionAudio.Get());
    TestTrue(TEXT("结束关卡后清除短音事件"),P->SeenReactionEvents.IsEmpty() && P->CancelledReactionEvents.IsEmpty());
    P->MulticastPlayReaction_Implementation(EDeliveryNpcVoiceReaction::Hit,FGuid::NewGuid(),P->ServerTime());
    TestNull(TEXT("结束后迟到的通知不能重建音频组件"),P->ReactionAudio.Get());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcDialogueVoiceAssetsTest,"Delivery.AI.DialogueVoiceAssets",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcDialogueVoiceAssetsTest::RunTest(const FString&)
{
    auto* Table=LoadObject<UDataTable>(nullptr,TEXT("/Game/NPC/Residents/Config/DT_NpcProfiles"));
    auto* Attenuation=LoadObject<USoundAttenuation>(nullptr,TEXT("/Game/NPC/Residents/Audio/SA_NpcDialogue"));
    if (!TestNotNull(TEXT("正式人物表存在"),Table) || !TestNotNull(TEXT("正式台词衰减资产存在"),Attenuation)) return false;
    TestTrue(TEXT("正式台词使用空间声音和距离衰减"),Attenuation->Attenuation.bAttenuate && Attenuation->Attenuation.bSpatialize);
    TestTrue(TEXT("正式台词覆盖交谈边缘与第三人称镜头"),Attenuation->Attenuation.Evaluate(FTransform::Identity,FVector(1000,0,0))>.6f);
    for (const FName RowName:{FName(TEXT("chunlai")),FName(TEXT("xiumei"))})
    {
        const auto* Profile=Table->FindRow<FDeliveryNpcProfileRow>(RowName,TEXT("DialogueVoiceAssets"),false);
        if (!TestNotNull(TEXT("人物行存在"),Profile) || !TestNotNull(TEXT("人物行仍引用声音资产"),Profile->VoiceSet.Get())) return false;
        TestTrue(TEXT("人物声音资产引用正式台词衰减"),Profile->VoiceSet->DialogueAttenuation==Attenuation);
        TestTrue(TEXT("正式短音同样覆盖第三人称交谈距离"),Profile->VoiceSet->ReactionAttenuation==Attenuation);
        TestNotNull(TEXT("正式人物配置了听见玩家的短音"),Profile->VoiceSet->HeardPlayer.Get());
        TestNotNull(TEXT("正式人物配置了受击短音"),Profile->VoiceSet->Hit.Get());
        TestTrue(TEXT("正式台词音量有效"),FMath::IsFinite(Profile->VoiceSet->DialogueVolume) &&
            Profile->VoiceSet->DialogueVolume>=0.f && Profile->VoiceSet->DialogueVolume<=2.f);
    }
    return true;
}
#endif
