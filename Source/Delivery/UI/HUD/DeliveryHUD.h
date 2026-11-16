#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Game/DeliveryStartupFlow.h"
#include "DeliveryHUD.generated.h"

class ADeliveryCharacter;
class ADeliveryNpcCharacter;
class UDeliveryConversationWidget;
class UDeliveryHotbarWidget;
class UDeliveryInventoryComponent;
class UDeliveryPhoneController;
class UDeliveryStartupWidget;
class UDeliveryTrunkStatusWidget;
class UDeliverySpeechInputComponent;
class UDeliverySpeechStatusWidget;
class UDeliveryAiInspectorWidget;
class UDeliveryAiInspectorController;
class UDeliveryMinimapWidget;
enum class EDeliverySpeechInputState : uint8;

/**
 * @brief 本机 HUD：创建和摆放各个界面，记着哪些面板开着，按面板决定鼠标显示和输入模式
 *
 * BeginPlay 依次创建开始菜单、手机处理器、语音状态图标和 AI 观察面板；Tick 每 0.1 秒刷新物品栏、后备箱状态和各界面的位置
 * 交谈面板由 OpenResidentConversation 创建、CloseResidentConversation 移除
 * 面板开关都经过 SetPanelOpen：有面板开着时显示鼠标并用 GameAndUI 模式，开始菜单开着时用 UIOnly，全关时回到 GameOnly
 * 控件只管布局和显示，按钮背后的操作由各界面处理器负责
 */
UCLASS()
class DELIVERY_API ADeliveryHUD : public AHUD
{
    GENERATED_BODY()
public:
    // ── 生命周期 ──

    /// @brief 每 0.1 秒更新一次；设置物品栏、交谈面板、语音状态图标和 AI 观察面板的默认 WBP
    ADeliveryHUD();

    // ── 交谈面板 ──

    /// @brief 关掉旧的交谈面板，新建 WBP_Conversation、调用 SetResident，让 AI 观察面板看这位 NPC，登记面板打开，再把焦点给输入框
    void OpenResidentConversation(ADeliveryNpcCharacter* Resident);

    /// @brief 登记交谈面板关闭（恢复输入模式），再把面板移出视口
    void CloseResidentConversation();

    // ── AI 观察面板 ──

    /// @brief F6：展开或收起 AI 观察面板，再调用 RefreshAiInspectorLayout 重新摆放
    void ToggleAiInspector();

    // ── 生命周期 ──

    /// @brief 每 0.1 秒：应用还没生效的输入模式，刷新物品栏和后备箱状态，摆放交谈面板、语音状态图标和 AI 观察面板
    virtual void Tick(float DeltaSeconds) override;

    // ── 物品栏 ──

    /// @brief 步行时返回这个角色，骑车时返回车上的驾驶员；物品栏和后备箱状态都读它
    static ADeliveryCharacter* ResolveCharacter(APawn* Pawn);

    // ── 面板与鼠标 ──

    /// @brief 手机处理器；第一次调用时在本机创建
    UFUNCTION(BlueprintPure, Category="UI")
    UDeliveryPhoneController* GetPhoneController();

    /// @brief 登记面板打开或关闭；从有面板变成没有（或反过来）、或开始菜单开关时，调用 ApplyInputMode 切换鼠标和输入模式
    void SetPanelOpen(FName PanelId, bool bOpen);

    /// @brief 名为 PanelId 的面板是否开着
    bool IsPanelOpen(FName PanelId) const { return OpenPanels.Contains(PanelId); }

    /// @brief 调试命令用：开着的面板名字，只读
    const TSet<FName>& GetOpenPanelsForDebug() const { return OpenPanels; }

    /// @brief 是否有面板开着
    bool IsAnyPanelOpen() const { return !OpenPanels.IsEmpty(); }

    /// @brief 语音状态图标是否已经在视口里；交谈面板开麦前先查它，没有图标时不开麦
    bool HasSpeechStatusWidget() const;
protected:
    // ── 生命周期 ──

    /// @brief 依次调用 InitializeStartupMenu、GetPhoneController、InitializeSpeechStatus、InitializeAiInspector
    virtual void BeginPlay() override;

    /// @brief 关掉所有界面和处理器，关闭麦克风，清空面板记录；之后不再请求鼠标或键盘焦点
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // ── 配置 ──

    /// 物品栏 WBP，没填时用 UDeliveryHotbarWidget
    UPROPERTY(EditDefaultsOnly, Category="UI|Hotbar")
    TSoftClassPtr<UDeliveryHotbarWidget> HotbarWidgetClass;

    /// 交谈面板 WBP
    UPROPERTY(EditDefaultsOnly, Category="UI|Conversation")
    TSoftClassPtr<UDeliveryConversationWidget> ConversationWidgetClass;

    /// 语音状态图标 WBP
    UPROPERTY(EditDefaultsOnly, Category="UI|Speech")
    TSoftClassPtr<UDeliverySpeechStatusWidget> SpeechStatusWidgetClass;

    /// AI 观察面板 WBP，只在非 Shipping 版本创建
    UPROPERTY(EditDefaultsOnly, Category="UI|AI Inspector")
    TSoftClassPtr<UDeliveryAiInspectorWidget> AiInspectorWidgetClass;
private:
    // ── AI 观察面板 ──

    /// @brief 非 Shipping 本机：创建观察面板和处理器 UDeliveryAiInspectorController，放在右上角
    void InitializeAiInspector();

    /// @brief 把观察面板放在小地图下方，底部避开交谈面板，告诉面板最多能占多大
    void RefreshAiInspectorLayout();

    /// 观察面板
    UPROPERTY(Transient) TObjectPtr<UDeliveryAiInspectorWidget> AiInspectorWidget;

    /// 观察面板处理器
    UPROPERTY(Transient) TObjectPtr<UDeliveryAiInspectorController> AiInspectorController;

    /// 本玩家的小地图，观察面板放在它下面；主界面可能稍后才建好，找不到时每 0.5 秒再找一次
    TWeakObjectPtr<UDeliveryMinimapWidget> InspectorMinimap;

    /// 下次找小地图的时间
    double NextInspectorMinimapSearchAt = 0;

    // ── 语音状态图标 ──

    /// @brief 本机：订阅麦克风组件的状态变化，创建语音状态图标放在左下角
    void InitializeSpeechStatus();

    /// @brief 把语音状态图标放在物品栏上方；和交谈面板重叠时移到交谈面板上方
    void RefreshSpeechStatusLayout();

    /// @brief 麦克风状态变化：没有语音状态图标时关掉麦克风，玩家不会在看不到提示的情况下被收音
    void HandleSpeechStateChanged(EDeliverySpeechInputState State);

    /// 语音状态图标
    UPROPERTY(Transient) TObjectPtr<UDeliverySpeechStatusWidget> SpeechStatusWidget;

    /// 订阅了状态变化的麦克风组件
    TWeakObjectPtr<UDeliverySpeechInputComponent> BoundSpeechInput;
    FDelegateHandle SpeechStateHandle;

    // ── 交谈面板 ──

    /// @brief 交谈面板宽度取屏幕宽的 52%（440 到 720），居中放在物品栏上方
    void RefreshConversationLayout();

    /// 交谈面板，没开时为空
    UPROPERTY(Transient) TObjectPtr<UDeliveryConversationWidget> ConversationWidget;

    // ── 开始菜单 ──

    /// @brief 本机且还没开局时：订阅 ADeliveryStartupFlow 的阶段变化，登记 Startup 面板打开，创建开始菜单
    void InitializeStartupMenu();

    /// @brief 进入 Playing 阶段时，下一帧调用 CloseStartupMenu；这一帧先让菜单自己也收到 Playing
    UFUNCTION()
    void HandleStartupPhase(EDeliveryStartupPhase Phase);

    /// @brief 登记 Startup 面板关闭（恢复输入模式），再移除开始菜单
    void CloseStartupMenu();

    /// 下一帧关闭开始菜单的计时器
    FTimerHandle StartupCloseTimer;

    /// 订阅了阶段变化的开局流程
    TWeakObjectPtr<ADeliveryStartupFlow> StartupFlow;

    /// 开始菜单
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryStartupWidget> StartupWidget;

    // ── 面板与鼠标 ──

    /// @brief 按 IsAnyPanelOpen 设置是否显示鼠标，记下要切换输入模式，再调用 TryApplyInputMode
    void ApplyInputMode();

    /// @brief 游戏窗口有焦点时才切换输入模式：开始菜单开着用 UIOnly，有面板开着用 GameAndUI，全关用 GameOnly
    void TryApplyInputMode();

    /// @brief 游戏窗口在前台、键盘焦点在这个视口里时返回 true；PIE 嵌在编辑器里时不能只看编辑器窗口
    bool CanApplyInputMode() const;

    /// 有一次输入模式切换还没生效；窗口在后台时只记下最新的，重新获得焦点后在 Tick 里应用
    bool bInputModePending = false;

    /// EndPlay 已经开始，之后不再请求焦点
    bool bEndingPlay = false;

    /// 开着的面板名字；同时开着几个时，关掉一个不收起鼠标
    TSet<FName> OpenPanels;

    /// 手机处理器
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryPhoneController> PhoneController;

    // ── 物品栏 ──

    /// @brief 本机：找到当前角色（骑车时是驾驶员），第一次时创建物品栏，背包换了就重新绑定；角色还没同步时隐藏
    void RefreshHotbar();

    /// @brief 玩家躺在后备箱里时显示后备箱状态条（文字和进度），否则隐藏
    void RefreshTrunkStatus();

    /// 后备箱状态条
    UPROPERTY(Transient) TObjectPtr<UDeliveryTrunkStatusWidget> TrunkStatus;

    /// @brief 移除物品栏并清空绑定；换了角色时调用，同一个角色上下车时复用
    void RemoveHotbar();

    /// 物品栏
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryHotbarWidget> HotbarWidget;

    /// 物品栏正在显示的背包
    TWeakObjectPtr<UDeliveryInventoryComponent> BoundInventory;

    /// 物品栏所属的角色
    TWeakObjectPtr<ADeliveryCharacter> BoundCharacter;
};
