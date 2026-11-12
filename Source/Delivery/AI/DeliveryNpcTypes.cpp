#include "AI/DeliveryNpcTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    FString Id(const FGuid& Value) { return Value.ToString(EGuidFormats::DigitsWithHyphensLower); }

    bool ReadId(const FJsonObject& Object, const TCHAR* Key, FGuid& Out, bool bOptional = false)
    {
        FString Value;
        if (!Object.TryGetStringField(Key, Value)) return false;
        if (bOptional && Value.IsEmpty()) { Out.Invalidate(); return true; }
        return FGuid::ParseExact(Value, EGuidFormats::DigitsWithHyphens, Out) && Out.IsValid();
    }

    bool ReadRevision(const FJsonObject& Object, const TCHAR* Key, int32& Out)
    {
        double Value = 0;
        if (!Object.TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) ||
            Value < 0 || Value > MAX_int32 || FMath::FloorToDouble(Value) != Value) return false;
        Out = static_cast<int32>(Value);
        return true;
    }
}

bool DeliveryNpc::IsTerminal(EDeliveryNpcActionStatus Status)
{
    return Status == EDeliveryNpcActionStatus::Succeeded || Status == EDeliveryNpcActionStatus::Failed ||
        Status == EDeliveryNpcActionStatus::Cancelled;
}

FString DeliveryNpc::ActionName(EDeliveryNpcAction Action)
{
    if (Action == EDeliveryNpcAction::Wait) return TEXT("Wait");
    if (Action == EDeliveryNpcAction::LookAt) return TEXT("LookAt");
    for (auto Value : {EDeliveryNpcAction::MoveTo,EDeliveryNpcAction::StrikeOnce,EDeliveryNpcAction::HoldVehicle,EDeliveryNpcAction::ReleaseVehicle,EDeliveryNpcAction::LieDown,EDeliveryNpcAction::GetUp})
        if (Action==Value) return StaticEnum<EDeliveryNpcAction>()->GetNameStringByValue(int64(Value));
    return TEXT("None");
}

FString DeliveryNpc::EncodeRequest(const FDeliveryNpcDecisionRequest& Request)
{
    auto Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("protocol_version"), FDeliveryNpcDecisionRequest::ProtocolVersion);
    Root->SetNumberField(TEXT("capability_version"), FDeliveryNpcDecisionRequest::CapabilityVersion);
    Root->SetStringField(TEXT("world_epoch"), Id(Request.WorldEpoch));
    Root->SetStringField(TEXT("npc_id"), Id(Request.NpcId));
    Root->SetStringField(TEXT("request_id"), Id(Request.RequestId));
    Root->SetNumberField(TEXT("context_revision"), Request.ContextRevision);
    Root->SetNumberField(TEXT("plan_revision"), Request.PlanRevision);
    Root->SetStringField(TEXT("persona"), Request.Persona);
    TArray<TSharedPtr<FJsonValue>> Actions;
    for (auto Action : Request.AllowedActions) Actions.Add(MakeShared<FJsonValueString>(ActionName(Action)));
    Root->SetArrayField(TEXT("allowed_actions"), Actions);
    TArray<TSharedPtr<FJsonValue>> Evidence;
    for (const auto& Entry : Request.Evidence)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("event_id"), Id(Entry.EventId));
        Item->SetStringField(TEXT("subject_id"), Entry.SubjectId.IsValid() ? Id(Entry.SubjectId) : TEXT(""));
        Item->SetStringField(TEXT("kind"), Entry.Kind.ToString());
        Item->SetStringField(TEXT("source"), StaticEnum<EDeliveryNpcEvidenceSource>()->GetNameStringByValue(static_cast<int64>(Entry.Source)));
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
    TArray<TSharedPtr<FJsonValue>> Candidates;
    for (const auto& Entry : Request.Candidates)
    {
        auto Item=MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("action"),ActionName(Entry.Action));
        Item->SetStringField(TEXT("target_id"),Entry.TargetId.IsValid()?Id(Entry.TargetId):TEXT(""));
        Item->SetNumberField(TEXT("duration"),Entry.Action==EDeliveryNpcAction::StrikeOnce?1.2:4.0);
        TArray<TSharedPtr<FJsonValue>> Refs;
        for (const auto& Ref : Entry.EvidenceIds) Refs.Add(MakeShared<FJsonValueString>(Id(Ref)));
        Item->SetArrayField(TEXT("evidence_ids"),Refs);
        Candidates.Add(MakeShared<FJsonValueObject>(Item));
    }
    Root->SetArrayField(TEXT("candidates"),Candidates);
    FString Result;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Result));
    return Result;
}

bool DeliveryNpc::DecodeProposal(const FString& Json, FDeliveryNpcDecisionProposal& OutProposal, FName& Reason)
{
    Reason = TEXT("InvalidSchema");
    if (Json.Len() > 65536) { Reason = TEXT("ResponseTooLarge"); return false; }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
    int32 Protocol = 0, Capability = 0;
    FDeliveryNpcDecisionProposal Parsed;
    if (!ReadRevision(*Root, TEXT("protocol_version"), Protocol) || Protocol != FDeliveryNpcDecisionRequest::ProtocolVersion ||
        !ReadRevision(*Root, TEXT("capability_version"), Capability) || Capability != FDeliveryNpcDecisionRequest::CapabilityVersion ||
        !ReadId(*Root, TEXT("world_epoch"), Parsed.WorldEpoch) || !ReadId(*Root, TEXT("npc_id"), Parsed.NpcId) ||
        !ReadId(*Root, TEXT("request_id"), Parsed.RequestId) || !ReadId(*Root, TEXT("target_id"), Parsed.TargetId, true) ||
        !ReadRevision(*Root, TEXT("context_revision"), Parsed.ContextRevision) ||
        !ReadRevision(*Root, TEXT("plan_revision"), Parsed.PlanRevision)) return false;

    FString Action;
    if (!Root->TryGetStringField(TEXT("action"), Action)) return false;
    if (Action == TEXT("Wait")) Parsed.Action = EDeliveryNpcAction::Wait;
    else if (Action == TEXT("LookAt")) Parsed.Action = EDeliveryNpcAction::LookAt;
    else
    {
        for (auto Value : {EDeliveryNpcAction::MoveTo,EDeliveryNpcAction::StrikeOnce,EDeliveryNpcAction::HoldVehicle,EDeliveryNpcAction::ReleaseVehicle,EDeliveryNpcAction::LieDown,EDeliveryNpcAction::GetUp})
            if (Action==ActionName(Value)) Parsed.Action=Value;
        if (Parsed.Action==EDeliveryNpcAction::None) { Reason=TEXT("UnsupportedAction"); return false; }
    }
    if (Root->HasField(TEXT("speech")) && (!Root->TryGetStringField(TEXT("speech"),Parsed.Speech) || Parsed.Speech.Len()>120)) return false;
    FString Emotion;
    if (Root->TryGetStringField(TEXT("emotion"),Emotion))
    {
        if (Emotion!=TEXT("Calm") && Emotion!=TEXT("Angry") && Emotion!=TEXT("Surprised")) return false;
        Parsed.Emotion=FName(*Emotion);
    }
    double Duration = 0;
    if (!Root->TryGetNumberField(TEXT("duration"), Duration) || !FMath::IsFinite(Duration) || Duration < .1 || Duration > 5) return false;
    Parsed.Duration = static_cast<float>(Duration);
    const TArray<TSharedPtr<FJsonValue>>* Evidence = nullptr;
    if (!Root->TryGetArrayField(TEXT("evidence_ids"), Evidence) || Evidence->Num() > 16) return false;
    for (const auto& Value : *Evidence)
    {
        FString Text;
        FGuid EventId;
        if (!Value->TryGetString(Text) || !FGuid::ParseExact(Text, EGuidFormats::DigitsWithHyphens, EventId) ||
            !EventId.IsValid() || Parsed.EvidenceIds.Contains(EventId)) return false;
        Parsed.EvidenceIds.Add(EventId);
    }
    OutProposal = MoveTemp(Parsed);
    Reason = NAME_None;
    return true;
}
