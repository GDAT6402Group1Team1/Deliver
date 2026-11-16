#pragma once

#include "CoreMinimal.h"
#include "World/Location/DeliveryLocationRegistry.h"
#include "AI/Conversation/DeliveryNpcMessageTypes.h"
#include "AI/Companion/Home/DeliveryCompanionHomeTypes.h"
#include "AI/Companion/Errand/DeliveryCompanionItemTypes.h"
#include "DeliveryNpcTypes.generated.h"
class APlayerState;
class APlayerController;
class APawn;
class UDeliveryTaskDefinition;
class ADeliveryPackageItem;

/**
 * @brief NPC 能做的全部动作，模型只能从这里面选
 *
 * Agent 每次问模型前，按当时的情况列出其中可做的几个当候选；模型选一个，服务器再检查一遍才执行
 * 注释以二狗开头的动作只有同伴能用，而且要主人发话
 */
UENUM(BlueprintType)
enum class EDeliveryNpcAction : uint8
{
    /// 没有动作
    None,

    /// 原地稍等，常和台词一起出现
    Wait,

    /// 看向某人或某物
    LookAt,

    /// 沿导航走近目标，真走到了才算完成
    MoveTo,

    /// 走近后打一拳，打没打中由服务器判定
    StrikeOnce,

    /// 拽住玩家的电瓶车，让车变慢
    HoldVehicle,

    /// 松开拽着的车
    ReleaseVehicle,

    /// 自己躺下（不是被打倒）
    LieDown,

    /// 自己躺下后爬起来
    GetUp,

    /// 在附近来回踱步再回到原处
    Pace,

    /// 往旁边挪开给人让路
    MoveAside,

    /// 走近并连续出拳；街坊可由模型选，二狗只能由服务器的护主规则安排
    Fight,

    /// 躲开威胁，跑到够远的地方
    Flee,

    /// 二狗：主人让它一直跟着，步行或骑自己的车
    Follow,

    /// 二狗：主人让它在原地附近等着
    Stay,

    /// 二狗：主人取消当前的委托，改成原地等
    CancelCompanionTask,

    /// 二狗：替主人去取货点领这一单的包裹，再拿回来交给主人
    FetchPackage,

    /// 二狗：从主人手上或车上接过这一单的包裹
    TakePackage,

    /// 二狗：替主人把包裹送到收件人那里，奖励由任务系统确认
    DeliverPackage,

    /// 二狗：回到主人身边，把包裹还给他
    ReturnPackage,

    /// 二狗：向主人提议往快递里塞一只小绿（毛毛虫恶作剧）
    OfferCaterpillar,

    /// 二狗：回到已解锁的共用房屋，到了就等着
    GoHome,

    /// 二狗：用家里的某件家具，比如坐下、开关、拿起、摆放
    HomeAction,

    /// 不换动作，只说句话，原来的安排接着做
    ContinueCurrent,

    /// 二狗：去拿主人指定的物品，再拿回来交给主人
    FetchItem,

    /// 二狗：把手上的物品交给主人
    ReturnItem,

    /// 二狗：骑车跟着主人时被挡住，按一下喇叭
    SoundFollowHorn,

    /// 二狗：把手上的包裹放在附近地上，任务进度不变，玩家可以捡回
    DropPackage,

    /// 二狗：从主人手上或背包里接过一件物品
    TakeItem,

    /// 二狗：去场景里捡一只小绿
    CollectCaterpillar,

    /// 二狗：主人同意后，把小绿塞进这一单的包裹，送达时有特殊奖励
    InsertCaterpillar,

    /// 二狗：主人打人或挨打后，加入帮主人打同一个人
    JoinAssist,

    /// 二狗：主人这一轮明确说收手，停止帮打
    StopAssist,

    /// 二狗：把手上的包裹放进主人的背包
    StorePackageInBackpack,

    /// 二狗：在主人附近走几个点找东西，看到什么报告什么
    SearchNearOwner
};

/// 动作进行到哪：空闲、已采纳、正在做、成功、失败、被取消
UENUM(BlueprintType)
enum class EDeliveryNpcActionStatus : uint8 { Idle, Accepted, Running, Succeeded, Failed, Cancelled };

/// 这个动作是谁安排的：模型选的，还是服务器按游戏规则直接安排的（比如二狗护主）
/// 两种走同一套执行逻辑，来源只由服务器填写
UENUM(BlueprintType)
enum class EDeliveryNpcActionOrigin : uint8 { Model, GameplayRule };

/// NPC 怎么知道这件事的：亲眼看见、亲耳听见、别人告诉的、自己做完动作的结果
UENUM(BlueprintType)
enum class EDeliveryNpcEvidenceSource : uint8 { Seen, Heard, ToldBy, ActionResult };

/// 二狗跟随时怎么走：步行、骑自己的车、自己看着办；Unspecified 表示这次没改原来的选择
UENUM(BlueprintType)
enum class EDeliveryCompanionTravelMode : uint8 { Unspecified, Walk, OwnVehicle, Automatic };

/// 主人让二狗安静点还是正常说话；只跟着 ContinueCurrent 候选出现
enum class EDeliveryCompanionSpeechPreference : uint8 { Unspecified, Quiet, Normal };

/// 二狗看到的这一单收件人家门：没看到、完好、已经被劈开（第一单要劈门才能交货）
enum class EDeliveryCompanionTaskDoorState : uint8 { Unknown, Intact, Broken };

/**
 * @brief NPC 亲身知道的一件事：看到谁做了什么、听到谁说了什么
 *
 * 角色在 HearSpeech、受击、看见有人冲过来时填一份交给 Agent 的 ObserveEvent；
 * NPC 没看到、没听到的事不会记进来，所以模型不会知道它不该知道的事
 */
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcObservation
{
    GENERATED_BODY()

    /// 事件编号，同一件事只记一次
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FGuid EventId;

    /// 这件事是在回答哪一句话，界面据此区分这是在回答玩家，还是 NPC 之间闲聊
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FGuid ReplyToEventId;

    /// 事件种类，比如 Hit（挨打）、Horn（听到喇叭）、SelfSpeech（自己说过的话）
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Kind;

    /// 怎么知道的
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EDeliveryNpcEvidenceSource Source = EDeliveryNpcEvidenceSource::Seen;

    /// 事件的主角：说话的人、打人的人、按喇叭的车
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TObjectPtr<AActor> Subject = nullptr;

    /// 这句话是对谁说的，旁边的 NPC 听到也知道不是对自己说的
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TObjectPtr<AActor> Addressee = nullptr;

    /// 有没有记下发生的位置；听别人说的事没有位置
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bHasObservedLocation = false;

    /// 发生的位置
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector ObservedLocation = FVector::ZeroVector;

    /// 这件事的文字描述；玩家说的话原样放在这里
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Description;

    /// 记多久，过了就忘
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0.1", ClampMax="60", Units="s"))
    float Lifetime = 10.f;

    /// 为 false 时只记住，不打断 NPC 正在说的回复，比如旁边 NPC 在闲聊；不暴露给蓝图
    bool bNeedsReply = true;
};

/**
 * @brief NPC 当前在做的动作，服务器同步给所有玩家，客户端据此播动画、显示台词
 *
 * Agent 采纳模型建议后写入，行为树按它执行；NPC 的记忆和发给模型的内容只留在服务器，不在这里
 */
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcPublicState
{
    GENERATED_BODY()

    /// 动作编号，执行和结束都靠它对上
    UPROPERTY(BlueprintReadOnly) FGuid ActionId;

    /// 回复编号；打架时模型可能说好几句，换台词不换动作编号，所以和 ActionId 分开
    UPROPERTY(BlueprintReadOnly) FGuid ReplyId;

    /// @brief 回复编号；没有单独的回复编号时用动作编号
    FGuid GetReplyId() const { return ReplyId.IsValid()?ReplyId:ActionId; }

    /// 计划修订号，每采纳一次新安排加一
    UPROPERTY(BlueprintReadOnly) int32 PlanRevision = 0;

    /// 做什么
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcAction Action = EDeliveryNpcAction::None;

    /// 谁安排的
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcActionOrigin Origin = EDeliveryNpcActionOrigin::Model;

    /// 规则动作的版本号，主人取消护主后加一，旧的规则动作就失效；模型改不了它
    UPROPERTY(BlueprintReadOnly) int32 RuleRevision = 0;

    /// 进行到哪
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcActionStatus Status = EDeliveryNpcActionStatus::Idle;

    /// 动作对象：要走向、要打、要看的人或物
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> Target = nullptr;

    /// 台词是对谁说的，可以和动作对象不同
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> SpeechTarget = nullptr;

    /// 开始的服务器时间，客户端据此对齐动画
    UPROPERTY(BlueprintReadOnly) double ServerStartTime = 0;

    /// 最多做多久
    UPROPERTY(BlueprintReadOnly) float Duration = 0;

    /// 结束或失败的原因
    UPROPERTY(BlueprintReadOnly) FName Reason;

    /// 台词全文
    UPROPERTY(BlueprintReadOnly) FString Speech;

    /// 在回答哪一句话
    UPROPERTY(BlueprintReadOnly) FGuid ReplyToEventId;

    /// 情绪，决定表情
    UPROPERTY(BlueprintReadOnly) FName Emotion;

    /// 台词是一句句边合成边播的
    UPROPERTY(BlueprintReadOnly) bool bStreamSpeech = false;

    /// 这个动作是替哪位玩家做的，只在服务器上；判断能不能做看它，不看台词是对谁说的
    UPROPERTY(Transient,NotReplicated) TObjectPtr<APlayerState> Requester = nullptr;

    /// 二狗安排的版本号，主人每下一次新安排加一，旧的作废
    int32 CompanionCommandRevision = 0;

    /// 协战的版本号，和普通安排分开计数，所以让二狗跟随或搬东西不会顺带取消帮打
    int32 CompanionAssistRevision = 0;

    /// 二狗这次怎么走
    EDeliveryCompanionTravelMode CompanionTravelMode=EDeliveryCompanionTravelMode::Unspecified;

    /// 二狗跑腿针对的是哪一单，服务器在请求时记下；模型只能选候选，不能换成别的单
    UPROPERTY(Transient,NotReplicated) TObjectPtr<UDeliveryTaskDefinition> CompanionTask = nullptr;

    /// 二狗跑腿针对的那件包裹
    UPROPERTY(Transient,NotReplicated) TObjectPtr<ADeliveryPackageItem> CompanionPackage = nullptr;

    /// 二狗选中的家具和位置，只在服务器上
    TSharedPtr<FDeliveryCompanionHomeChoice> HomeChoice;

    /// 二狗选中的物品
    TSharedPtr<FDeliveryCompanionItemChoice> ItemChoice;
};

/// Agent 记住的一件事，准备发给模型的版本：人和物都换成编号，不带 UE 对象指针
struct DELIVERY_API FDeliveryNpcEvidence
{
    /// 事件编号
    FGuid EventId;

    /// 事件主角的编号
    FGuid SubjectId;

    /// 说话对象的编号
    FGuid AddresseeId;

    /// 在回答哪一句话
    FGuid ReplyToEventId;

    /// 事件种类
    FName Kind;

    /// 怎么知道的
    EDeliveryNpcEvidenceSource Source = EDeliveryNpcEvidenceSource::Seen;

    /// 文字描述，发给模型
    FString Description;

    /// 有没有位置
    bool bHasObservedLocation = false;

    /// 发生的位置
    FVector ObservedLocation = FVector::ZeroVector;

    /// 什么时候知道的
    double ObservedAt = 0;

    /// 什么时候忘掉
    double ExpiresAt = 0;

    /// 为 false 时只记住，不打断正在说的回复；这个字段不发给模型
    bool bNeedsReply = true;
};

/// 列给模型的一个候选：做什么、对谁、根据哪件事；二狗的还带着具体是哪件物品、哪件家具
struct FDeliveryNpcCandidate
{
    /// 候选编号，模型回答时报这个编号
    FGuid CandidateId;

    /// 做什么
    EDeliveryNpcAction Action=EDeliveryNpcAction::Wait;

    /// 对象编号
    FGuid TargetId;

    /// 根据哪几件事，模型回答时要引用其中的
    TArray<FGuid> EvidenceIds;

    /// 二狗：具体哪件家具和位置
    TSharedPtr<FDeliveryCompanionHomeChoice> HomeChoice;

    /// 二狗：具体哪件物品
    TSharedPtr<FDeliveryCompanionItemChoice> ItemChoice;

    /// 二狗：怎么走
    EDeliveryCompanionTravelMode TravelMode=EDeliveryCompanionTravelMode::Unspecified;

    /// 二狗：安静点还是正常说话
    EDeliveryCompanionSpeechPreference SpeechPreference=EDeliveryCompanionSpeechPreference::Unspecified;
};

/// 模型能用编号指代的一个人或物，只包括这位 NPC 见过的
struct FDeliveryNpcSubject
{
    /// 编号
    FGuid SubjectId;

    /// 名字
    FString Name;

    /// 种类，比如 player、vehicle、other
    FString Kind;
};

/// 二狗记得的一件往事，比如主人救过它；只当聊天背景，不能作为刚才有人打了主人的证据
struct DELIVERY_API FDeliveryNpcCompanionMemory
{
    /// 往事编号
    FGuid EventId;

    /// 往事种类
    FName Kind;

    /// 往事描述
    FString Description;
};

/// 二狗刚看到的一个地点，模型能在台词里提到它，但不能因此让二狗走过去
struct DELIVERY_API FDeliveryNpcObservedPlace
{
    /// 地点编号
    FString PlaceId;

    /// 地点名
    FString Name;

    /// 地点描述
    FString Description;

    /// 离二狗多远，厘米
    float DistanceCm=0;
};

/// 二狗记得的一件家具：名字和描述，不含现在的状态和位置
struct FDeliveryNpcRememberedHomeObject
{
    /// 家具名
    FString Name;

    /// 家具描述
    FString Description;
};

/**
 * @brief 问模型一次要带的全部内容：我是谁、身边有谁、刚发生了什么、能做哪些动作、正在做什么
 *
 * Agent 的 BuildDecisionRequest 填写，DeliveryNpc::EncodeRequest 转成 JSON 交给网关发给后端
 * 以 Running 开头的字段是请求发出时 NPC 正在做的事，回答回来时据此判断情况变没变；这些只留在服务器
 */
struct DELIVERY_API FDeliveryNpcDecisionRequest
{
    // ── 只留在服务器、不发给模型 ──

    /// 这次请求在接另一位 NPC 的哪句话
    FDeliveryNpcMessageHandle ParentNpcMessage;

    /// 发请求时正在做的动作；回答回来时动作已经换了，就不采纳
    FGuid RunningActionId;

    /// 请求格式的版本
    static constexpr int32 ProtocolVersion = 1;

    /// 游戏支持的最高协议版本
    static constexpr int32 CapabilityVersion = 10;

    /// 这次请求要的协议版本：街坊用 4，二狗按需要用更高的版本
    int32 RequestedCapabilityVersion = 4;

    /// 替哪位玩家问的；由服务器填写，不看模型说是在对谁说话
    TWeakObjectPtr<APlayerController> RequesterPC;

    /// 替哪位玩家问的（玩家状态，换角色也不变）
    TWeakObjectPtr<APlayerState> RequesterPlayerState;

    /// 玩家说的那句话的编号
    FGuid PlayerMessageId;

    /// 二狗初遇时的回应：针对哪次救它的事、说给哪位主人
    FGuid JoinedResponseEvent;

    /// 主人在模型眼里的编号
    FGuid JoinedOwnerSubjectId;

    /// 主人当时控制的角色
    TWeakObjectPtr<APawn> JoinedOwnerPawn;

    /// 正在做的动作的计划修订号
    int32 RunningPlanRevision=0;

    /// 正在做的动作的对象
    TWeakObjectPtr<AActor> RunningTarget;

    /// 正在做的动作是替谁做的
    TWeakObjectPtr<APlayerState> RunningRequester;

    /// 正在做的动作是谁安排的
    EDeliveryNpcActionOrigin RunningOrigin=EDeliveryNpcActionOrigin::Model;

    /// 正在做的动作什么时候开始
    double RunningStartTime=0;

    /// 正在做的动作预计多久
    float RunningDuration=0;

    /// 正在做的动作最晚什么时候结束
    double RunningDeadline=0;

    /// 二狗正在执行主人的安排
    bool bRunningCompanion=false;

    /// 二狗正在跑腿
    bool bRunningErrand=false;

    /// 二狗正在摆弄家具
    bool bRunningHome=false;

    /// 二狗当时的模式：跟随、等候等
    uint8 RunningCompanionMode=0;

    /// 二狗当时的主人
    TWeakObjectPtr<APlayerState> RunningCompanionOwner;

    /// 当时的家具安排编号
    FGuid RunningHomeArrangement;

    /// 当时拿着的家具
    TWeakObjectPtr<AActor> RunningHomeProp;

    /// 当时手上的物品
    TWeakObjectPtr<AActor> RunningHeldItem;

    /// 当时跑腿要拿的物品
    TWeakObjectPtr<AActor> RunningErrandItem;

    /// 世界编号
    FGuid WorldEpoch;

    /// NPC 编号
    FGuid NpcId;

    /// 这次请求的编号
    FGuid RequestId;

    /// 记忆版本，换主人等大变动后旧请求作废
    int32 ContextRevision = 0;

    /// 计划修订号
    int32 PlanRevision = 0;

    /// 人设
    FString Persona;

    // ── 发给模型的内容 ──

    /// 人物表行名，后端据此找这位 NPC 的背景资料；没选人物时为空
    FString ProfileId;

    /// 自己的名字
    FString SelfName;

    /// 自己在模型眼里的编号
    FGuid SelfSubjectId;

    /// 能用编号指代的人和物
    TArray<FDeliveryNpcSubject> Subjects;

    /// 记得的事
    TArray<FDeliveryNpcEvidence> Evidence;

    /// 上次采纳的回答用到了哪些事，模型据此知道哪些是新发生的
    TArray<FGuid> LastDecisionEventIds;

    /// 第几层接话，只在游戏里用，不发给模型
    int32 ConversationDepth = 0;

    /// 这次允许的动作种类
    TArray<EDeliveryNpcAction> AllowedActions;

    /// 这次的候选动作
    TArray<FDeliveryNpcCandidate> Candidates;

    /// 候选编号 → 下标，候选列完时建好，查的时候不打乱给模型的顺序
    TMap<FGuid,int32> CandidateIndexById;

    /// @brief 按编号找候选，找不到返回空
    const FDeliveryNpcCandidate* FindCandidate(FGuid CandidateId) const
    {
        const int32* Index=CandidateIndexById.Find(CandidateId);
        return Index && Candidates.IsValidIndex(*Index) && Candidates[*Index].CandidateId==CandidateId?
            &Candidates[*Index]:nullptr;
    }

    /// 是不是在回答玩家
    bool bPlayerRequest = false;

    /// 要求后端一句话边合成边分段发回来；游戏和后端都开了才生效
    bool bStreamSentenceAudio = false;

    /// 流式协议版本；版本 2 分开通知两件事：模型的文字已经全部生成，和声音已经全部合成；0 用旧协议
    int32 StreamProtocolVersion = 0;

    /// 还有玩家的话没回答
    bool bPendingPlayerMessage = false;

    /// 正在做的动作
    EDeliveryNpcAction RunningAction=EDeliveryNpcAction::Wait;

    /// 正在做的动作的对象编号
    FGuid RunningTargetId;

    /// 正在做的动作还剩几秒
    double RunningSecondsRemaining=0;

    /// 合成台词用的音色
    FString Voice = TEXT("male");

    // ── 二狗专用 ──
    // 主人、关系、正在做的安排、手上拿着什么、车在哪；全由服务器填写

    /// 有没有下面这些二狗的信息
    bool bHasCompanionContext = false;

    /// 主人的名字
    FString CompanionOwnerName;

    /// 和主人的关系到了哪一步
    FString CompanionRelationshipStage;

    /// 现在能不能提议塞小绿
    bool bCanOfferCaterpillar=false;

    /// 记得的往事
    TArray<FDeliveryNpcCompanionMemory> CompanionMemories;

    /// 屋里现在能用的家具
    TArray<FDeliveryNpcHomeObjectView> HomeObjects;

    /// 以前见过的家具，只有名字和描述
    TArray<FDeliveryNpcRememberedHomeObject> RememberedHomeObjects;

    /// 刚看到的地点
    TArray<FDeliveryNpcObservedPlace> CompanionObservedPlaces;

    /// 熟悉的地点，比如共用房屋
    TArray<FPublicNpcPlace> FamiliarPlaces;

    /// 这次说话的是不是主人；不是主人只能聊天，不能下命令
    bool bRequesterIsCompanionOwner = false;

    /// 当前模式
    FString CompanionMode;

    /// 安排暂停的原因
    FString CompanionSuspensionReason;

    /// 安排的版本号
    int32 CompanionCommandRevision = 0;

    /// 协战的版本号
    int32 CompanionAssistRevision = 0;

    /// 已经答应帮打
    bool bCompanionAssistCommitted = false;

    /// 有一次帮打等着确认
    bool bCompanionAssistPending = false;

    /// 主人当前订单的编号
    FString CompanionTaskId;

    /// 订单进行到哪：待取件、配送中等
    FString CompanionTaskPhase;

    /// 订单标题
    FString CompanionTaskTitle;

    /// 订单描述
    FString CompanionTaskDescription;

    /// 收件人家门的状态
    EDeliveryCompanionTaskDoorState CompanionTaskDoorState=EDeliveryCompanionTaskDoorState::Unknown;

    /// 跟随时怎么走
    FString CompanionTravelMode;

    /// 跑腿时怎么走
    FString CompanionErrandTravelMode;

    /// 二狗的车在哪、有没有人骑
    FString CompanionVehicleState;

    /// 主人的车在哪
    FString CompanionOwnerVehicleState;

    /// 主人让它安静，还剩几秒
    int32 CompanionQuietSecondsRemaining=0;

    /// 二狗自己的车能不能骑
    bool bCompanionOwnVehicleAvailable=false;

    /// 主人手上或车上有没有这单的包裹
    bool bCompanionOwnerHasPackage = false;

    /// 二狗手上有没有包裹
    bool bCompanionHasPackage = false;

    /// 二狗手上物品的编号
    FGuid CompanionHeldItemId;

    /// 二狗手上物品的名字
    FString CompanionHeldItemName;

    /// 二狗手上的东西放在哪：手上、车上等
    FName CompanionCarriedItemLocation;

    /// 主人的包裹放在哪
    FName CompanionOwnerPackageLocation;

    /// 正在跑的腿是哪个动作
    FString CompanionErrandAction;

    /// 跑腿针对哪一单
    FString CompanionErrandTaskId;

    /// 跑腿进行到哪一步
    FString CompanionErrandPhase;

    /// 跑腿的状态
    FString CompanionErrandStatus;

    /// 跑腿失败或暂停的原因
    FString CompanionErrandReason;

    /// 这一单的任务和包裹本身，只在服务器采纳和执行前核对用，不发给模型
    TWeakObjectPtr<UDeliveryTaskDefinition> CompanionTask;

    /// 这一单的包裹本身
    TWeakObjectPtr<ADeliveryPackageItem> CompanionPackage;
};

/// 后端发回的一整句：文字和完整声音
/// 几句可能同时合成、先后不定地到，靠序号排回正确的播放顺序
struct FDeliveryNpcSentence
{
    /// 第几句，0 开始
    int32 Sequence = 0;

    /// 台词
    FString Text;

    /// 情绪
    FName Emotion;

    /// 完整的原始声音
    TArray<uint8> Pcm;

    /// 采样率
    int32 SampleRate = 16000;
};

/// 边合成边发时一句话的开头：先发文字，后面的声音分段按这个序号对上
struct FDeliveryNpcSpeechBegin
{
    /// 第几句
    int32 Sequence = 0;

    /// 台词
    FString Text;

    /// 情绪
    FName Emotion;

    /// 采样率
    int32 SampleRate = 24000;

    /// 分段格式版本
    int32 AudioStreamVersion = 1;
};

/// 一句话里的一段原始声音，按句子序号和段序号拼起来
struct FDeliveryNpcAudioChunk
{
    /// 第几句
    int32 Sequence = 0;

    /// 第几段
    int32 ChunkIndex = 0;

    /// 这段在整句里从第几个字节开始，用来发现丢段
    int32 OffsetBytes = 0;

    /// 这段原始声音
    TArray<uint8> Pcm;
};

/// 一句话的声音发完了；合成失败也会发这条，这时表现组件改用文字和电子音播这句
struct FDeliveryNpcAudioEnd
{
    /// 第几句
    int32 Sequence = 0;

    /// 一共几段
    int32 ChunkCount = 0;

    /// 整句原始声音的字节数，用来核对收全没有
    int32 TotalPcmBytes = 0;

    /// 合成成功
    bool bSucceeded = false;

    /// 失败原因
    FString Error;
};

/**
 * @brief 模型的回答：选了哪个候选、对谁、说什么、什么情绪、根据哪几件事
 *
 * 网关从后端的 JSON 解出来，交给 Agent 的 TryAcceptProposal 逐项检查，通过才执行
 * 编号和修订号都要和请求时一致，对不上说明情况已经变了
 */
struct DELIVERY_API FDeliveryNpcDecisionProposal
{
    /// 回答用的协议版本
    int32 CapabilityVersion = 4;

    /// 选中的候选编号
    FGuid CandidateId;

    /// 世界编号，要和请求一致
    FGuid WorldEpoch;

    /// NPC 编号，要和请求一致
    FGuid NpcId;

    /// 请求编号，要和请求一致
    FGuid RequestId;

    /// 记忆版本，要和请求一致
    int32 ContextRevision = 0;

    /// 计划修订号，要和请求一致
    int32 PlanRevision = 0;

    /// 选中的动作
    EDeliveryNpcAction Action = EDeliveryNpcAction::None;

    /// 动作对象编号
    FGuid TargetId;

    /// 台词对谁说
    FGuid SpeechTargetId;

    /// 有没有指定说话对象
    bool bHasSpeechTarget = false;

    /// 打算做多久
    float Duration = 0;

    /// 台词
    FString Speech;

    /// 情绪
    FName Emotion;

    /// 根据哪几件事，必须是请求里给过的
    TArray<FGuid> EvidenceIds;
};

/// 动作分类、名字、时长上限，以及请求和回答的 JSON 转换；实现在 DeliveryNpcTypes.cpp
namespace DeliveryNpc
{
    /// 打架时每次最多安排 8 秒，到时再问模型要不要继续
    constexpr float FightDecisionSeconds=8.f;

    /// 其他动作每次最多 5 秒
    constexpr float OtherActionSeconds=5.f;

    /// 连续打架最长 20 秒
    constexpr float MaxContinuousCombatSeconds=20.f;

    /// 打架快结束前 4 秒就先问下一步，免得中间停顿
    constexpr float CombatDecisionLeadSeconds=4.f;

    /// @brief 这个动作每次最多安排多久
    inline float MaxDecisionDuration(EDeliveryNpcAction Action)
    { return Action==EDeliveryNpcAction::Fight?FightDecisionSeconds:OtherActionSeconds; }
    /// @brief 动作是否已经结束（成功、失败或取消）
    DELIVERY_API bool IsTerminal(EDeliveryNpcActionStatus Status);

    /// @brief 动作的英文名，和发给模型的名字一致
    DELIVERY_API FString ActionName(EDeliveryNpcAction Action);

    /// @brief 是不是要主人发话的二狗动作
    DELIVERY_API bool IsCompanionCommand(EDeliveryNpcAction Action);

    /// @brief 是不是二狗跑腿类的动作
    DELIVERY_API bool IsCompanionErrand(EDeliveryNpcAction Action);

    /// @brief 是不是拿取或交还普通物品的跑腿
    DELIVERY_API bool IsCompanionItemErrand(EDeliveryNpcAction Action);

    /// @brief 这个动作作为候选时最多做多久；二狗的持续安排没有时长，返回 0
    DELIVERY_API float CandidateDuration(EDeliveryNpcAction Action);

    /// @brief 把请求转成发给后端的 JSON
    DELIVERY_API FString EncodeRequest(const FDeliveryNpcDecisionRequest& Request);

    /// @brief 把后端返回的 JSON 解成回答
    /// @return 格式不对时返回 false，原因写在 Reason
    DELIVERY_API bool DecodeProposal(const FString& Json, FDeliveryNpcDecisionProposal& OutProposal, FName& Reason);
}
