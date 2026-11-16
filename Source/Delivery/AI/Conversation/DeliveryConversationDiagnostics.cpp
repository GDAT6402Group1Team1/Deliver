#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "UI/HUD/DeliveryHUD.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"

// ── 检查器 ──
// 开发时按快捷键打开 AI 检查器，可以看到附近一位 NPC 这次向模型问了什么、模型怎么回答、游戏有没有采纳
// 本机调用 WatchNpcDiagnostics，它调用 ServerWatchNpcDiagnostics；服务器订阅这位 NPC 的 OnDecisionTraceChanged，并调用 ClientNpcDiagnostics 推回当前记录
// 之后记录每次变化，服务器在 HandleInspectorTrace 里再调用 ClientNpcDiagnostics
// 本机的 ClientNpcDiagnostics_Implementation 丢掉过期的记录，再调用 OnDiagnosticsChanged.Broadcast 交给检查器界面显示
// Shipping 版本不编译这些代码

/// @brief 本机：给打开检查器的按键绑定回调，并加上对应的输入映射
/// 换了 InputComponent 或输入子系统（例如换了角色）时，先解掉旧的绑定再绑新的
void UDeliveryConversationComponent::BindInspectorInput()
{
#if !UE_BUILD_SHIPPING
    auto* PC = Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController())
        return;
    auto* Input = Cast<UEnhancedInputComponent>(PC->InputComponent);
    auto* Action = LoadObject<UInputAction>(nullptr, TEXT("/Game/UI/Conversation/Input/IA_ToggleAiInspector"));
    auto* Mapping = LoadObject<UInputMappingContext>(nullptr, TEXT("/Game/UI/Conversation/Input/IMC_AiInspector"));
    if (!Input || !Action || !Mapping)
        return;

    // PC->InputComponent 换了，或者 InspectorInputHandle 为 0（还没绑过），才重新绑定 IA_ToggleAiInspector
    if (BoundInspectorInput.Get() != Input || !InspectorInputHandle)
    {
        if (auto* Old = BoundInspectorInput.Get(); Old && InspectorInputHandle)
            Old->RemoveBindingByHandle(InspectorInputHandle);
        BoundInspectorInput = Input;
        InspectorInputHandle =
            Input->BindAction(Action, ETriggerEvent::Started, this, &UDeliveryConversationComponent::ToggleInspector).GetHandle();
    }
    if (auto* Sub = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
    {
        // 输入子系统换了：bOwnsInspectorMapping 为 true 时从旧子系统删掉 InspectorMapping，再记下新的子系统
        if (InspectorInputSubsystem.Get() != Sub || InspectorMapping.Get() != Mapping)
        {
            if (auto* Old = InspectorInputSubsystem.Get(); Old && bOwnsInspectorMapping && InspectorMapping.IsValid())
                Old->RemoveMappingContext(InspectorMapping.Get());
            InspectorInputSubsystem = Sub;
            InspectorMapping = Mapping;
            bOwnsInspectorMapping = false;
        }

        // IMC_AiInspector 还没加上才加，并把 bOwnsInspectorMapping 设为 true；ShutdownInspector 只删这个组件自己加的映射
        if (!Sub->HasMappingContext(Mapping))
        {
            Sub->AddMappingContext(Mapping, 1);
            bOwnsInspectorMapping = true;
        }
    }
#endif
}

/// @brief 按下检查器按键时，请 HUD 打开或关闭检查器界面
void UDeliveryConversationComponent::ToggleInspector()
{
    if (const auto* PC = Cast<APlayerController>(GetOwner()))
        if (auto* HUD = Cast<ADeliveryHUD>(PC->GetHUD()))
            HUD->ToggleAiInspector();
}

/// @brief 判断能不能观察这位 NPC：30 米以内就行，倒地或正在跑动的也能看
bool UDeliveryConversationComponent::CanInspectNpc(const ADeliveryNpcCharacter* Resident) const
{
    const auto* PC = Cast<APlayerController>(GetOwner());
    const APawn* Pawn = PC ? PC->GetPawn() : nullptr;

    // 检查器只用于开发调试，所以不调用 CanTalkTo，只看距离 3000 cm 以内
    return !bEndingPlay && Pawn && IsValid(Resident) && Resident->GetWorld() == GetWorld() && Resident->Agent &&
           FVector::DistSquared(Pawn->GetActorLocation(), Resident->GetActorLocation()) <= FMath::Square(3000.f);
}

/// @brief 本机：开始观察一位 NPC，先给界面一份只有名字的空记录，再请服务器开始推送
void UDeliveryConversationComponent::WatchNpcDiagnostics(ADeliveryNpcCharacter* Resident)
{
#if !UE_BUILD_SHIPPING
    const auto* PC = Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController() || !IsValid(Resident) || Resident->GetWorld() != GetWorld())
        return;

    // LocalInspectorTarget 已经是这位 NPC，LocalInspectorPawn 也没换，不再调用 ServerWatchNpcDiagnostics
    if (LocalInspectorTarget.Get() == Resident && LocalInspectorPawn.Get() == PC->GetPawn())
        return;
    LocalInspectorTarget = Resident;
    LocalInspectorPawn = PC->GetPawn();

    // 清掉上一位 NPC 的 InspectorWorldEpoch 和 InspectorRevision，下一份记录从头开始比较
    InspectorWorldEpoch.Invalidate();
    InspectorRevision = -1;

    // LocalInspectorGeneration 加一；ClientNpcDiagnostics 收到的 Generation 和它不同时，记录不显示
    if (++LocalInspectorGeneration == 0)
        ++LocalInspectorGeneration;
    FDeliveryNpcDecisionTrace Empty;
    Empty.NpcName = Resident->DisplayName.ToString();
    OnDiagnosticsChanged.Broadcast(Empty);
    ServerWatchNpcDiagnostics(Resident, LocalInspectorGeneration);
#endif
}

/// @brief 本机：停止观察；观察次数加一，路上还没到的旧记录到了也不显示
void UDeliveryConversationComponent::StopWatchingNpcDiagnostics()
{
    if (++LocalInspectorGeneration == 0)
        ++LocalInspectorGeneration;
    LocalInspectorTarget.Reset();
    LocalInspectorPawn.Reset();
    InspectorWorldEpoch.Invalidate();
    InspectorRevision = -1;
    if (!bEndingPlay)
        ServerWatchNpcDiagnostics(nullptr, LocalInspectorGeneration);
    OnDiagnosticsChanged.Broadcast(FDeliveryNpcDecisionTrace());
}

/// @brief 服务器：不再订阅这位 NPC 的决策记录，清掉观察对象
void UDeliveryConversationComponent::UnbindServerInspector()
{
    if (auto* Npc = ServerInspectorTarget.Get(); Npc && Npc->Agent)
        Npc->Agent->OnDecisionTraceChanged.Remove(InspectorTraceHandle);
    InspectorTraceHandle.Reset();
    ServerInspectorTarget.Reset();
    ServerInspectorPawn.Reset();
}

/// @brief 服务器：玩家请求观察某位 NPC 时，订阅它的决策记录，并马上推送一份当前记录
/// @param Resident 要观察的 NPC，空表示停止观察
/// @param Generation 本机第几次开始观察，比已处理过的旧就忽略
void UDeliveryConversationComponent::ServerWatchNpcDiagnostics_Implementation(ADeliveryNpcCharacter* Resident, uint64 Generation)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || bEndingPlay || !Generation || Generation <= ServerInspectorGeneration)
        return;
    ServerInspectorGeneration = Generation;
    UnbindServerInspector();
#if !UE_BUILD_SHIPPING

    // CanInspectNpc 不通过：推一份 Stage 为 Unavailable 的空记录，界面据此显示这位 NPC 无法查看
    if (!CanInspectNpc(Resident))
    {
        FDeliveryNpcDecisionTrace Empty;
        Empty.Stage = Resident ? FName(TEXT("Unavailable")) : NAME_None;
        ClientNpcDiagnostics(Generation, Empty);
        return;
    }
    ServerInspectorTarget = Resident;
    ServerInspectorPawn = CastChecked<APlayerController>(GetOwner())->GetPawn();

    // 绑定 OnDecisionTraceChanged，之后这位 NPC 每次更新决策记录都会调用 HandleInspectorTrace
    InspectorTraceHandle =
        Resident->Agent->OnDecisionTraceChanged.AddUObject(this, &UDeliveryConversationComponent::HandleInspectorTrace);
    FDeliveryNpcDecisionTrace Initial = Resident->Agent->GetDecisionTrace();
    if (Initial.NpcName.IsEmpty())
        Initial.NpcName = Resident->DisplayName.ToString();
    ClientNpcDiagnostics(Generation, Initial);
#endif
}

/// @brief 服务器：NPC 的决策记录变了，推送给观察的玩家；玩家走远或换了角色就停止推送
void UDeliveryConversationComponent::HandleInspectorTrace(const FDeliveryNpcDecisionTrace& Trace)
{
    auto* PC = Cast<APlayerController>(GetOwner());
    auto* Npc = ServerInspectorTarget.Get();
    if (!PC || !PC->HasAuthority())
        return;

    // 玩家走远或换了角色：调用 UnbindServerInspector 取消订阅，再推一份 Stage 为 Unavailable 的空记录
    if (!CanInspectNpc(Npc) || ServerInspectorPawn.Get() != PC->GetPawn())
    {
        UnbindServerInspector();
        FDeliveryNpcDecisionTrace Empty;
        Empty.Stage = TEXT("Unavailable");
        if (!bEndingPlay)
            ClientNpcDiagnostics(ServerInspectorGeneration, Empty);
        return;
    }
    ClientNpcDiagnostics(ServerInspectorGeneration, Trace);
}

/// @brief 本机：收到服务器推来的决策记录，丢掉过期的，再交给检查器界面
/// @param Generation 这份记录属于第几次观察，和本机当前的次数不同就丢掉
/// 同一位 NPC 的记录还要比世界编号和版本号：别的世界来的、或者比已显示的旧，都不显示
void UDeliveryConversationComponent::ClientNpcDiagnostics_Implementation(uint64 Generation,
                                                                         const FDeliveryNpcDecisionTrace& Trace)
{
    const auto* PC = Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController() || Generation != LocalInspectorGeneration || !PC->GetPawn() ||
        LocalInspectorPawn.Get() != PC->GetPawn())
        return;

    // 只有真实的决策记录带 RequestId；WatchNpcDiagnostics 和服务器推来的空记录没有 RequestId，不比较世界编号和版本号
    if (Trace.RequestId.IsValid())
    {
        if (!LocalInspectorTarget.IsValid() || LocalInspectorTarget->GetWorld() != GetWorld() || !Trace.WorldEpoch.IsValid())
            return;

        // InspectorWorldEpoch 已经记下时，WorldEpoch 不同的记录来自另一个世界，不显示
        if (InspectorWorldEpoch.IsValid() && InspectorWorldEpoch != Trace.WorldEpoch)
            return;

        // Revision 不大于 InspectorRevision，说明是迟到的旧记录，不显示
        if (Trace.Revision <= InspectorRevision)
            return;
        InspectorWorldEpoch = Trace.WorldEpoch;
        InspectorRevision = Trace.Revision;
    }

    // Stage 为 Unavailable 时清掉 LocalInspectorTarget，下次调用 WatchNpcDiagnostics 会重新请求这位 NPC
    if (Trace.Stage == TEXT("Unavailable"))
        LocalInspectorTarget.Reset();
    OnDiagnosticsChanged.Broadcast(Trace);
}

/// @brief 组件结束时解除检查器的全部绑定：服务器的订阅、本机的按键和输入映射
void UDeliveryConversationComponent::ShutdownInspector()
{
    UnbindServerInspector();
    if (auto* Input = BoundInspectorInput.Get(); Input && InspectorInputHandle)
        Input->RemoveBindingByHandle(InspectorInputHandle);
    if (auto* Sub = InspectorInputSubsystem.Get(); Sub && bOwnsInspectorMapping && InspectorMapping.IsValid())
        Sub->RemoveMappingContext(InspectorMapping.Get());
    BoundInspectorInput.Reset();
    InspectorInputSubsystem.Reset();
    InspectorMapping.Reset();
    InspectorInputHandle = 0;
    bOwnsInspectorMapping = false;
    LocalInspectorTarget.Reset();
    LocalInspectorPawn.Reset();
    OnDiagnosticsChanged.Clear();
}
