#include "AI/Decision/DeliveryNpcTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"

namespace
{
// 编号转成带连字符的小写字符串，与后端格式一致
FString Id(const FGuid& Value)
{
    return Value.ToString(EGuidFormats::DigitsWithHyphensLower);
}

// 读取一个编号字段
bool ReadId(const FJsonObject& Object, const TCHAR* Key, FGuid& Out, bool bOptional = false)
{
    FString Value;
    if (!Object.TryGetStringField(Key, Value))
        return false;
    if (bOptional && Value.IsEmpty())
    {
        Out.Invalidate();
        return true;
    }
    return FGuid::ParseExact(Value, EGuidFormats::DigitsWithHyphens, Out) && Out.IsValid();
}

// 读取一个非负整数版本号
bool ReadRevision(const FJsonObject& Object, const TCHAR* Key, int32& Out)
{
    double Value = 0;
    if (!Object.TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) || Value < 0 || Value > MAX_int32 ||
        FMath::FloorToDouble(Value) != Value)
        return false;
    Out = static_cast<int32>(Value);
    return true;
}
} // namespace

// 动作状态是否已经结束
bool DeliveryNpc::IsTerminal(EDeliveryNpcActionStatus Status)
{
    return Status == EDeliveryNpcActionStatus::Succeeded || Status == EDeliveryNpcActionStatus::Failed ||
           Status == EDeliveryNpcActionStatus::Cancelled;
}

// 是否是同伴专用的指令类动作
bool DeliveryNpc::IsCompanionCommand(EDeliveryNpcAction Action)
{
    return Action == EDeliveryNpcAction::Follow || Action == EDeliveryNpcAction::Stay ||
           Action == EDeliveryNpcAction::CancelCompanionTask || Action == EDeliveryNpcAction::OfferCaterpillar ||
           Action == EDeliveryNpcAction::GoHome || Action == EDeliveryNpcAction::HomeAction ||
           Action == EDeliveryNpcAction::JoinAssist || Action == EDeliveryNpcAction::StopAssist || IsCompanionErrand(Action);
}

// 是否是同伴的跑腿类动作
bool DeliveryNpc::IsCompanionErrand(EDeliveryNpcAction Action)
{
    return Action == EDeliveryNpcAction::FetchPackage || Action == EDeliveryNpcAction::TakePackage ||
           Action == EDeliveryNpcAction::DeliverPackage || Action == EDeliveryNpcAction::ReturnPackage ||
           Action == EDeliveryNpcAction::DropPackage || Action == EDeliveryNpcAction::StorePackageInBackpack ||
           IsCompanionItemErrand(Action);
}

// 是否是同伴的取物类动作
bool DeliveryNpc::IsCompanionItemErrand(EDeliveryNpcAction Action)
{
    return Action == EDeliveryNpcAction::FetchItem || Action == EDeliveryNpcAction::ReturnItem ||
           Action == EDeliveryNpcAction::TakeItem || Action == EDeliveryNpcAction::CollectCaterpillar ||
           Action == EDeliveryNpcAction::InsertCaterpillar || Action == EDeliveryNpcAction::SearchNearOwner;
}

// 每种动作的默认时长上限
float DeliveryNpc::CandidateDuration(EDeliveryNpcAction Action)
{
    if (Action == EDeliveryNpcAction::SoundFollowHorn)
        return .3f;
    if (IsCompanionCommand(Action) || Action == EDeliveryNpcAction::ContinueCurrent)
        return 0.f;
    const bool bNeedsMovementTime = Action == EDeliveryNpcAction::StrikeOnce || Action == EDeliveryNpcAction::Fight ||
                                    Action == EDeliveryNpcAction::Pace || Action == EDeliveryNpcAction::MoveAside ||
                                    Action == EDeliveryNpcAction::Flee;
    return bNeedsMovementTime ? MaxDecisionDuration(Action) : 4.f;
}

// 动作的协议名称，与后端和日志一致
FString DeliveryNpc::ActionName(EDeliveryNpcAction Action)
{
    if (Action == EDeliveryNpcAction::ContinueCurrent)
        return TEXT("ContinueCurrent");
    if (Action == EDeliveryNpcAction::SoundFollowHorn)
        return TEXT("SoundFollowHorn");
    if (Action == EDeliveryNpcAction::StorePackageInBackpack)
        return TEXT("StorePackageInBackpack");
    if (Action == EDeliveryNpcAction::SearchNearOwner)
        return TEXT("SearchNearOwner");
    if (Action == EDeliveryNpcAction::Wait)
        return TEXT("Wait");
    if (Action == EDeliveryNpcAction::LookAt)
        return TEXT("LookAt");
    for (auto Value : {EDeliveryNpcAction::MoveTo,
                       EDeliveryNpcAction::StrikeOnce,
                       EDeliveryNpcAction::HoldVehicle,
                       EDeliveryNpcAction::ReleaseVehicle,
                       EDeliveryNpcAction::LieDown,
                       EDeliveryNpcAction::GetUp,
                       EDeliveryNpcAction::Pace,
                       EDeliveryNpcAction::MoveAside,
                       EDeliveryNpcAction::Fight,
                       EDeliveryNpcAction::Flee,
                       EDeliveryNpcAction::Follow,
                       EDeliveryNpcAction::Stay,
                       EDeliveryNpcAction::CancelCompanionTask,
                       EDeliveryNpcAction::FetchPackage,
                       EDeliveryNpcAction::TakePackage,
                       EDeliveryNpcAction::DeliverPackage,
                       EDeliveryNpcAction::ReturnPackage,
                       EDeliveryNpcAction::OfferCaterpillar,
                       EDeliveryNpcAction::GoHome,
                       EDeliveryNpcAction::HomeAction,
                       EDeliveryNpcAction::FetchItem,
                       EDeliveryNpcAction::ReturnItem,
                       EDeliveryNpcAction::DropPackage,
                       EDeliveryNpcAction::TakeItem,
                       EDeliveryNpcAction::CollectCaterpillar,
                       EDeliveryNpcAction::InsertCaterpillar,
                       EDeliveryNpcAction::JoinAssist,
                       EDeliveryNpcAction::StopAssist})
        if (Action == Value)
            return StaticEnum<EDeliveryNpcAction>()->GetNameStringByValue(int64(Value));
    return TEXT("None");
}

// 把请求编码成 JSON，网关 Dispatch 时作为 POST 请求体
FString DeliveryNpc::EncodeRequest(const FDeliveryNpcDecisionRequest& Request)
{
    auto Root = MakeShared<FJsonObject>();

    // 协议版本和身份编号，后端原样带回，游戏用来核对回复
    Root->SetNumberField(TEXT("protocol_version"), FDeliveryNpcDecisionRequest::ProtocolVersion);
    Root->SetNumberField(TEXT("capability_version"), Request.RequestedCapabilityVersion);
    Root->SetStringField(TEXT("world_epoch"), Id(Request.WorldEpoch));
    Root->SetStringField(TEXT("npc_id"), Id(Request.NpcId));
    Root->SetStringField(TEXT("request_id"), Id(Request.RequestId));
    Root->SetNumberField(TEXT("context_revision"), Request.ContextRevision);
    Root->SetNumberField(TEXT("plan_revision"), Request.PlanRevision);
    Root->SetStringField(TEXT("persona"), Request.Persona);

    // 熟悉的地点、人物配置和同伴上下文
    TArray<TSharedPtr<FJsonValue>> FamiliarPlaces;
    for (const auto& Place : Request.FamiliarPlaces)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Place.Name);
        Item->SetStringField(TEXT("description"), Place.Description);
        FamiliarPlaces.Add(MakeShared<FJsonValueObject>(Item));
    }
    if (!FamiliarPlaces.IsEmpty())
        Root->SetArrayField(TEXT("familiar_places"), FamiliarPlaces);
    if (!Request.ProfileId.IsEmpty())
        Root->SetStringField(TEXT("profile_id"), Request.ProfileId);
    Root->SetStringField(TEXT("self_name"), Request.SelfName);
    Root->SetStringField(TEXT("self_subject_id"), Request.SelfSubjectId.IsValid() ? Id(Request.SelfSubjectId) : TEXT(""));
    if (Request.bHasCompanionContext)
    {
        auto Companion = MakeShared<FJsonObject>();
        Companion->SetStringField(TEXT("owner_name"), Request.CompanionOwnerName);
        Companion->SetBoolField(TEXT("requester_is_owner"), Request.bRequesterIsCompanionOwner);
        Companion->SetStringField(TEXT("mode"), Request.CompanionMode);
        Companion->SetStringField(TEXT("suspension_reason"), Request.CompanionSuspensionReason);
        Companion->SetNumberField(TEXT("command_revision"), Request.CompanionCommandRevision);
        if (Request.RequestedCapabilityVersion >= 10)
        {
            Companion->SetNumberField(TEXT("assist_revision"), Request.CompanionAssistRevision);
            Companion->SetBoolField(TEXT("assist_active"), Request.bCompanionAssistCommitted);
            Companion->SetBoolField(TEXT("assist_pending"), Request.bCompanionAssistPending);
        }
        if (Request.RequestedCapabilityVersion >= 7)
        {
            Companion->SetStringField(TEXT("held_item_id"),
                                      Request.CompanionHeldItemId.IsValid() ? Id(Request.CompanionHeldItemId) : TEXT(""));
            Companion->SetStringField(TEXT("held_item_name"), Request.CompanionHeldItemName);
        }
        Companion->SetStringField(TEXT("task_id"), Request.CompanionTaskId);
        Companion->SetStringField(TEXT("task_phase"), Request.CompanionTaskPhase);
        Companion->SetStringField(TEXT("task_title"), Request.CompanionTaskTitle);
        if (Request.RequestedCapabilityVersion >= 8)
        {
            Companion->SetStringField(TEXT("travel_mode"), Request.CompanionTravelMode);
            if (!Request.CompanionErrandTaskId.IsEmpty())
                Companion->SetStringField(TEXT("errand_task_id"), Request.CompanionErrandTaskId);
            if (Request.CompanionTaskDoorState == EDeliveryCompanionTaskDoorState::Intact)
                Companion->SetStringField(TEXT("task_door_state"), TEXT("Intact"));
            else if (Request.CompanionTaskDoorState == EDeliveryCompanionTaskDoorState::Broken)
                Companion->SetStringField(TEXT("task_door_state"), TEXT("Broken"));
            if (!Request.CompanionErrandTravelMode.IsEmpty())
                Companion->SetStringField(TEXT("errand_travel_mode"), Request.CompanionErrandTravelMode);
            if (!Request.CompanionVehicleState.IsEmpty())
                Companion->SetStringField(TEXT("vehicle_state"), Request.CompanionVehicleState);
            if (Request.bPlayerRequest && Request.bRequesterIsCompanionOwner)
            {
                if (Request.RequestedCapabilityVersion >= 10 && !Request.CompanionOwnerVehicleState.IsEmpty())
                    Companion->SetStringField(TEXT("owner_vehicle_state"), Request.CompanionOwnerVehicleState);
                if (!Request.CompanionCarriedItemLocation.IsNone() &&
                    (Request.bCompanionHasPackage || Request.CompanionHeldItemId.IsValid()))
                    Companion->SetStringField(TEXT("carried_item_location"), Request.CompanionCarriedItemLocation.ToString());
                if (!Request.CompanionOwnerPackageLocation.IsNone() &&
                    (Request.bCompanionOwnerHasPackage ||
                     (Request.RequestedCapabilityVersion >= 10 && Request.CompanionOwnerPackageLocation == TEXT("Ground"))))
                    Companion->SetStringField(TEXT("owner_package_location"), Request.CompanionOwnerPackageLocation.ToString());
            }
            if (Request.CompanionQuietSecondsRemaining > 0)
                Companion->SetNumberField(TEXT("quiet_seconds_remaining"), Request.CompanionQuietSecondsRemaining);
            Companion->SetBoolField(TEXT("own_vehicle_available"), Request.bCompanionOwnVehicleAvailable);
            Companion->SetStringField(TEXT("task_description"), Request.CompanionTaskDescription);
        }
        Companion->SetBoolField(TEXT("owner_has_package"), Request.bCompanionOwnerHasPackage);
        Companion->SetBoolField(TEXT("has_package"), Request.bCompanionHasPackage);
        Companion->SetStringField(TEXT("errand_action"), Request.CompanionErrandAction);
        Companion->SetStringField(TEXT("errand_phase"), Request.CompanionErrandPhase);
        Companion->SetStringField(TEXT("errand_status"), Request.CompanionErrandStatus);
        Companion->SetStringField(TEXT("errand_reason"), Request.CompanionErrandReason);
        Companion->SetStringField(TEXT("relationship_stage"), Request.CompanionRelationshipStage);
        Companion->SetBoolField(TEXT("can_offer_caterpillar"), Request.bCanOfferCaterpillar);
        TArray<TSharedPtr<FJsonValue>> Memories;
        for (const auto& Memory : Request.CompanionMemories)
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("event_id"), Id(Memory.EventId));
            Item->SetStringField(TEXT("kind"), Memory.Kind.ToString());
            Item->SetStringField(TEXT("description"), Memory.Description);
            Memories.Add(MakeShared<FJsonValueObject>(Item));
        }
        Companion->SetArrayField(TEXT("memories"), Memories);
        if (Request.RequestedCapabilityVersion >= 5)
        {
            TArray<TSharedPtr<FJsonValue>> Objects;
            for (const auto& Object : Request.HomeObjects)
            {
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("object_id"), Id(Object.ObjectId));
                Item->SetStringField(TEXT("name"), Object.Name);
                Item->SetStringField(TEXT("state"), Object.State);
                TArray<TSharedPtr<FJsonValue>> Uses;
                for (const FString& Use : Object.Uses)
                    Uses.Add(MakeShared<FJsonValueString>(Use));
                Item->SetArrayField(TEXT("uses"), Uses);
                TArray<TSharedPtr<FJsonValue>> Slots;
                for (const auto& Slot : Object.Slots)
                {
                    auto SlotItem = MakeShared<FJsonObject>();
                    SlotItem->SetStringField(TEXT("slot_id"), Slot.SlotId);
                    SlotItem->SetStringField(TEXT("name"), Slot.Name);
                    SlotItem->SetBoolField(TEXT("available"), Slot.bAvailable);
                    Slots.Add(MakeShared<FJsonValueObject>(SlotItem));
                }
                Item->SetArrayField(TEXT("slots"), Slots);
                Objects.Add(MakeShared<FJsonValueObject>(Item));
            }
            Companion->SetArrayField(TEXT("home_objects"), Objects);
        }
        if (Request.RequestedCapabilityVersion >= 8 && !Request.RememberedHomeObjects.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> Remembered;
            for (const auto& Object : Request.RememberedHomeObjects)
            {
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("name"), Object.Name);
                Item->SetStringField(TEXT("description"), Object.Description);
                Remembered.Add(MakeShared<FJsonValueObject>(Item));
            }
            Companion->SetArrayField(TEXT("remembered_home_objects"), Remembered);
        }
        TArray<TSharedPtr<FJsonValue>> ObservedPlaces;
        for (const auto& Place : Request.CompanionObservedPlaces)
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("place_id"), Place.PlaceId);
            Item->SetStringField(TEXT("name"), Place.Name);
            Item->SetStringField(TEXT("description"), Place.Description);
            Item->SetNumberField(TEXT("distance_cm"), Place.DistanceCm);
            ObservedPlaces.Add(MakeShared<FJsonValueObject>(Item));
        }
        Companion->SetArrayField(TEXT("observed_places"), ObservedPlaces);
        Root->SetObjectField(TEXT("companion_context"), Companion);
    }

    // 对象名单
    TArray<TSharedPtr<FJsonValue>> Subjects;
    for (const auto& Entry : Request.Subjects)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("subject_id"), Id(Entry.SubjectId));
        Item->SetStringField(TEXT("name"), Entry.Name);
        Item->SetStringField(TEXT("kind"), Entry.Kind);
        Subjects.Add(MakeShared<FJsonValueObject>(Item));
    }
    Root->SetArrayField(TEXT("subjects"), Subjects);

    // 请求来源和语音设置：player 表示回答玩家，ambient 表示自主行动
    Root->SetStringField(TEXT("request_source"), Request.bPlayerRequest ? TEXT("player") : TEXT("ambient"));
    Root->SetStringField(TEXT("voice"), Request.Voice);
    if (Request.bStreamSentenceAudio)
        Root->SetStringField(TEXT("speech_transport"), TEXT("pcm_chunks_v1"));
    if (Request.StreamProtocolVersion == 2)
        Root->SetNumberField(TEXT("stream_protocol_version"), 2);
    Root->SetBoolField(TEXT("pending_player_message"), Request.bPendingPlayerMessage);
    if ((Request.RequestedCapabilityVersion >= 6 && (Request.RunningActionId.IsValid() || Request.bRunningCompanion)) ||
        (Request.RunningActionId.IsValid() && Request.RunningTargetId.IsValid() && Request.RunningSecondsRemaining > 0 &&
         (Request.RunningAction == EDeliveryNpcAction::Fight || Request.RunningAction == EDeliveryNpcAction::Flee)))
    {
        auto Current = MakeShared<FJsonObject>();
        Current->SetStringField(TEXT("action"), ActionName(Request.RunningAction));
        Current->SetStringField(TEXT("target_id"), Request.RunningTargetId.IsValid() ? Id(Request.RunningTargetId) : TEXT(""));
        Current->SetStringField(TEXT("source"),
                                !Request.RunningActionId.IsValid() && Request.bRunningCompanion   ? TEXT("Companion")
                                : Request.RunningOrigin == EDeliveryNpcActionOrigin::GameplayRule ? TEXT("GameplayRule")
                                                                                                  : TEXT("Model"));
        Current->SetNumberField(TEXT("remaining_seconds"), Request.RunningSecondsRemaining);
        Current->SetStringField(TEXT("action_id"), Request.RunningActionId.IsValid() ? Id(Request.RunningActionId) : TEXT(""));
        if (Request.RequestedCapabilityVersion >= 6)
            Current->SetNumberField(TEXT("plan_revision"), Request.RunningPlanRevision);
        Root->SetObjectField(TEXT("current_action"), Current);
    }
    if (Request.RequestedCapabilityVersion >= 6 && Request.bRunningCompanion)
    {
        auto Activity = MakeShared<FJsonObject>();
        Activity->SetStringField(TEXT("kind"), TEXT("companion"));
        Activity->SetStringField(TEXT("mode"), Request.CompanionMode);
        Activity->SetStringField(TEXT("errand_action"), Request.CompanionErrandAction);
        Activity->SetStringField(TEXT("errand_phase"), Request.CompanionErrandPhase);
        Activity->SetBoolField(TEXT("has_home_arrangement"), Request.bRunningHome);
        Root->SetObjectField(TEXT("current_activity"), Activity);
    }

    // 允许的动作
    TArray<TSharedPtr<FJsonValue>> Actions;
    for (auto Action : Request.AllowedActions)
        Actions.Add(MakeShared<FJsonValueString>(ActionName(Action)));
    Root->SetArrayField(TEXT("allowed_actions"), Actions);

    // 事件列表，以及上一次已经读过的事件编号
    TArray<TSharedPtr<FJsonValue>> Evidence;
    for (const auto& Entry : Request.Evidence)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("event_id"), Id(Entry.EventId));
        Item->SetStringField(TEXT("reply_to_event_id"), Entry.ReplyToEventId.IsValid() ? Id(Entry.ReplyToEventId) : TEXT(""));
        Item->SetStringField(TEXT("subject_id"), Entry.SubjectId.IsValid() ? Id(Entry.SubjectId) : TEXT(""));
        Item->SetStringField(TEXT("addressee_id"), Entry.AddresseeId.IsValid() ? Id(Entry.AddresseeId) : TEXT(""));
        Item->SetStringField(TEXT("kind"), Entry.Kind.ToString());
        Item->SetStringField(TEXT("source"),
                             StaticEnum<EDeliveryNpcEvidenceSource>()->GetNameStringByValue(static_cast<int64>(Entry.Source)));
        if (Entry.Kind == TEXT("NpcMessage") || Entry.Kind == TEXT("NpcMessageSent"))
            Item->SetBoolField(TEXT("needs_reply"), Entry.Kind == TEXT("NpcMessage") && Entry.bNeedsReply);
        Item->SetStringField(TEXT("description"), Entry.Description);
        Item->SetNumberField(TEXT("observed_at"), Entry.ObservedAt);
        Item->SetNumberField(TEXT("expires_at"), Entry.ExpiresAt);
        Item->SetBoolField(TEXT("has_observed_location"), Entry.bHasObservedLocation);
        TArray<TSharedPtr<FJsonValue>> Location;
        for (double Value : {Entry.ObservedLocation.X, Entry.ObservedLocation.Y, Entry.ObservedLocation.Z})
            Location.Add(MakeShared<FJsonValueNumber>(Value));
        Item->SetArrayField(TEXT("observed_location"), Location);
        Evidence.Add(MakeShared<FJsonValueObject>(Item));
    }
    Root->SetArrayField(TEXT("evidence"), Evidence);
    TArray<TSharedPtr<FJsonValue>> LastDecisionEvents;
    for (const auto& EventId : Request.LastDecisionEventIds)
        LastDecisionEvents.Add(MakeShared<FJsonValueString>(Id(EventId)));
    Root->SetArrayField(TEXT("last_decision_event_ids"), LastDecisionEvents);

    // 候选列表，模型按序号选择
    TArray<TSharedPtr<FJsonValue>> Candidates;
    for (const auto& Entry : Request.Candidates)
    {
        auto Item = MakeShared<FJsonObject>();
        if (Request.RequestedCapabilityVersion >= 5)
        {
            Item->SetStringField(TEXT("candidate_id"), Id(Entry.CandidateId));
            if (Entry.Action == EDeliveryNpcAction::HomeAction && Entry.HomeChoice.IsValid())
            {
                const auto& Choice = *Entry.HomeChoice;
                auto Home = MakeShared<FJsonObject>();
                Home->SetStringField(TEXT("object_id"), Id(Choice.SourceId));
                Home->SetStringField(TEXT("use"),
                                     StaticEnum<EDeliveryHomeOperation>()->GetNameStringByValue(int64(Choice.Operation)));
                Home->SetStringField(TEXT("destination_object_id"),
                                     Choice.DestinationId.IsValid() ? Id(Choice.DestinationId) : TEXT(""));
                Home->SetStringField(TEXT("destination_slot_id"),
                                     Choice.DestinationSlotId.IsNone() ? TEXT("") : Choice.DestinationSlotId.ToString());
                Item->SetObjectField(TEXT("home"), Home);
            }
        }
        Item->SetStringField(TEXT("action"), ActionName(Entry.Action));
        if (Request.RequestedCapabilityVersion >= 10 && Entry.ItemChoice.IsValid() &&
            Entry.Action != EDeliveryNpcAction::SearchNearOwner)
        {
            Item->SetStringField(TEXT("item_source"), Entry.ItemChoice->Source.ToString());
            const auto* Package = Entry.ItemChoice->UsePackage.Get();
            const auto* DeliveryItem = IsValid(Package) ? Package->GetDeliveryItemComponent() : nullptr;
            if (DeliveryItem && DeliveryItem->OwningTask)
                Item->SetStringField(TEXT("package_task_id"), DeliveryItem->OwningTask->TaskId.ToString());
        }
        if (Request.RequestedCapabilityVersion >= 8 && Entry.TravelMode != EDeliveryCompanionTravelMode::Unspecified)
            Item->SetStringField(TEXT("travel_mode"),
                                 StaticEnum<EDeliveryCompanionTravelMode>()->GetNameStringByValue(int64(Entry.TravelMode)));
        if (Request.RequestedCapabilityVersion >= 8 && Entry.SpeechPreference != EDeliveryCompanionSpeechPreference::Unspecified)
            Item->SetStringField(TEXT("speech_preference"), Entry.SpeechPreference == EDeliveryCompanionSpeechPreference::Quiet
                                                                ? TEXT("Quiet")
                                                                : TEXT("Normal"));
        Item->SetStringField(TEXT("target_id"), Entry.TargetId.IsValid() ? Id(Entry.TargetId) : TEXT(""));
        Item->SetNumberField(TEXT("duration"), CandidateDuration(Entry.Action));
        TArray<TSharedPtr<FJsonValue>> Refs;
        for (const auto& Ref : Entry.EvidenceIds)
            Refs.Add(MakeShared<FJsonValueString>(Id(Ref)));
        Item->SetArrayField(TEXT("evidence_ids"), Refs);
        Candidates.Add(MakeShared<FJsonValueObject>(Item));
    }
    Root->SetArrayField(TEXT("candidates"), Candidates);
    FString Result;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Result));
    return Result;
}

// 解析后端返回的建议 JSON，字段缺失或越界时返回 false 并写入原因
bool DeliveryNpc::DecodeProposal(const FString& Json, FDeliveryNpcDecisionProposal& OutProposal, FName& Reason)
{
    Reason = TEXT("InvalidSchema");
    if (Json.Len() > 65536)
    {
        Reason = TEXT("ResponseTooLarge");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
        return false;
    int32 Protocol = 0, Capability = 0;
    FDeliveryNpcDecisionProposal Parsed;

    // 协议版本和身份编号
    if (!ReadRevision(*Root, TEXT("protocol_version"), Protocol) || Protocol != FDeliveryNpcDecisionRequest::ProtocolVersion ||
        !ReadRevision(*Root, TEXT("capability_version"), Capability) || Capability < 1 ||
        Capability > FDeliveryNpcDecisionRequest::CapabilityVersion || !ReadId(*Root, TEXT("world_epoch"), Parsed.WorldEpoch) ||
        !ReadId(*Root, TEXT("npc_id"), Parsed.NpcId) || !ReadId(*Root, TEXT("request_id"), Parsed.RequestId) ||
        !ReadId(*Root, TEXT("target_id"), Parsed.TargetId, true) ||
        !ReadRevision(*Root, TEXT("context_revision"), Parsed.ContextRevision) ||
        !ReadRevision(*Root, TEXT("plan_revision"), Parsed.PlanRevision))
        return false;
    Parsed.CapabilityVersion = Capability;
    if (Capability >= 5)
    {
        FString CandidateText;
        if (!ReadId(*Root, TEXT("candidate_id"), Parsed.CandidateId) ||
            !Root->TryGetStringField(TEXT("candidate_id"), CandidateText) || CandidateText != Id(Parsed.CandidateId))
            return false;
    }

    FString Action;

    // 动作、目标、说话对象、台词和情绪
    if (!Root->TryGetStringField(TEXT("action"), Action))
        return false;
    if (Action == TEXT("Wait"))
        Parsed.Action = EDeliveryNpcAction::Wait;
    else if (Action == TEXT("ContinueCurrent") && Capability >= 6)
        Parsed.Action = EDeliveryNpcAction::ContinueCurrent;
    else if (Action == TEXT("SoundFollowHorn") && Capability >= 8)
        Parsed.Action = EDeliveryNpcAction::SoundFollowHorn;
    else if (Action == TEXT("StorePackageInBackpack") && Capability >= 10)
        Parsed.Action = EDeliveryNpcAction::StorePackageInBackpack;
    else if (Action == TEXT("SearchNearOwner") && Capability >= 10)
        Parsed.Action = EDeliveryNpcAction::SearchNearOwner;
    else if (Action == TEXT("LookAt"))
        Parsed.Action = EDeliveryNpcAction::LookAt;
    else
    {
        for (auto Value : {EDeliveryNpcAction::MoveTo,
                           EDeliveryNpcAction::StrikeOnce,
                           EDeliveryNpcAction::HoldVehicle,
                           EDeliveryNpcAction::ReleaseVehicle,
                           EDeliveryNpcAction::LieDown,
                           EDeliveryNpcAction::GetUp,
                           EDeliveryNpcAction::Pace,
                           EDeliveryNpcAction::MoveAside,
                           EDeliveryNpcAction::Fight,
                           EDeliveryNpcAction::Flee,
                           EDeliveryNpcAction::Follow,
                           EDeliveryNpcAction::Stay,
                           EDeliveryNpcAction::CancelCompanionTask,
                           EDeliveryNpcAction::FetchPackage,
                           EDeliveryNpcAction::TakePackage,
                           EDeliveryNpcAction::DeliverPackage,
                           EDeliveryNpcAction::ReturnPackage,
                           EDeliveryNpcAction::OfferCaterpillar,
                           EDeliveryNpcAction::GoHome,
                           EDeliveryNpcAction::HomeAction,
                           EDeliveryNpcAction::FetchItem,
                           EDeliveryNpcAction::ReturnItem,
                           EDeliveryNpcAction::DropPackage,
                           EDeliveryNpcAction::TakeItem,
                           EDeliveryNpcAction::CollectCaterpillar,
                           EDeliveryNpcAction::InsertCaterpillar,
                           EDeliveryNpcAction::JoinAssist,
                           EDeliveryNpcAction::StopAssist})
            if (Action == ActionName(Value))
                Parsed.Action = Value;
        if (Parsed.Action == EDeliveryNpcAction::None)
        {
            Reason = TEXT("UnsupportedAction");
            return false;
        }
    }
    if (Capability < 10 &&
        (Parsed.Action == EDeliveryNpcAction::TakeItem || Parsed.Action == EDeliveryNpcAction::CollectCaterpillar ||
         Parsed.Action == EDeliveryNpcAction::InsertCaterpillar || Parsed.Action == EDeliveryNpcAction::JoinAssist ||
         Parsed.Action == EDeliveryNpcAction::StopAssist))
        return false;
    if (Capability < 9 && Parsed.Action == EDeliveryNpcAction::DropPackage)
        return false;
    if (Capability < 7 && IsCompanionItemErrand(Parsed.Action))
        return false;
    if (IsCompanionItemErrand(Parsed.Action) &&
        (Parsed.TargetId.IsValid() == (Parsed.Action == EDeliveryNpcAction::SearchNearOwner)))
        return false;
    if (Capability < 2 && IsCompanionCommand(Parsed.Action))
        return false;
    if (Capability < 3 && IsCompanionErrand(Parsed.Action))
        return false;
    if (Capability < 4 && (Parsed.Action == EDeliveryNpcAction::OfferCaterpillar || Parsed.Action == EDeliveryNpcAction::GoHome))
        return false;
    if (Parsed.Action == EDeliveryNpcAction::HomeAction && (Capability < 5 || Parsed.TargetId.IsValid()))
        return false;
    if (Root->HasField(TEXT("speech")) && (!Root->TryGetStringField(TEXT("speech"), Parsed.Speech) || Parsed.Speech.Len() > 120))
        return false;
    Parsed.bHasSpeechTarget = Root->HasField(TEXT("speech_target_id"));
    if (Parsed.bHasSpeechTarget && !ReadId(*Root, TEXT("speech_target_id"), Parsed.SpeechTargetId, true))
        return false;
    FString Emotion;
    if (Root->TryGetStringField(TEXT("emotion"), Emotion))
    {
        if (Emotion != TEXT("Calm") && Emotion != TEXT("Angry") && Emotion != TEXT("Surprised"))
            return false;
        Parsed.Emotion = FName(*Emotion);
    }
    double Duration = 0;

    // 时长和引用的事件，事件最多 16 条
    if (!Root->TryGetNumberField(TEXT("duration"), Duration) || !FMath::IsFinite(Duration) ||
        ((IsCompanionCommand(Parsed.Action) || Parsed.Action == EDeliveryNpcAction::ContinueCurrent)
             ? Duration != 0.0
             : (Duration < .1 || Duration > MaxDecisionDuration(Parsed.Action))))
        return false;
    Parsed.Duration = static_cast<float>(Duration);
    const TArray<TSharedPtr<FJsonValue>>* Evidence = nullptr;
    if (!Root->TryGetArrayField(TEXT("evidence_ids"), Evidence) || Evidence->Num() > 16)
        return false;
    for (const auto& Value : *Evidence)
    {
        FString Text;
        FGuid EventId;
        if (!Value->TryGetString(Text) || !FGuid::ParseExact(Text, EGuidFormats::DigitsWithHyphens, EventId) ||
            !EventId.IsValid() || Parsed.EvidenceIds.Contains(EventId))
            return false;
        Parsed.EvidenceIds.Add(EventId);
    }
    OutProposal = MoveTemp(Parsed);
    Reason = NAME_None;
    return true;
}
