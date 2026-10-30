// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UPostProcessComponent;
class UDeliveryDialogueVoiceComponent;
class UDeliveryMusicComponent;
class UAudioComponent;
class USoundBase;
class UDeliveryGroundArrowComponent;
class UDeliveryObjectiveRingComponent;
class UUserWidget;

/**
 * PlayerController that only installs Enhanced Input mapping contexts.
 */
UCLASS()
class ADeliveryPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	ADeliveryPlayerController();

	/** 当前任务的脚边方向箭头；上下摩托车时持续存在。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|Guidance")
	TObjectPtr<UDeliveryGroundArrowComponent> GroundArrow;

	/** 当前任务目的地的地面圈。和脚边箭头同位置、同一个目的地来源。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery")
	TObjectPtr<UDeliveryObjectiveRingComponent> ObjectiveRing;

	/** 本机人物电子语音生成器。电话/NPC UI 都复用这一份。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Voice")
	UDeliveryDialogueVoiceComponent* GetDialogueVoice() const { return DialogueVoice; }

	/** 背景音乐。和语音组件同一个位置：跨 Possess（上下摩托车）不断。 */
	UDeliveryMusicComponent* GetMusic() const { return Music; }

	/**
	 * 接听当前来电。客户端调用会自动转成 Server RPC。
	 *
	 * 电话队列挂在 GameState 上，而 GameState 不属于任何客户端，
	 * 客户端没法直接对它发 RPC——所以玩家的接听/挂断意图统一从这里中转。
	 */
	UFUNCTION(BlueprintCallable, Category="Phone")
	void RequestAnswerCall();

	/** 挂断。响铃时是拒接，通话中是提前结束。两种都会影响所有玩家。 */
	UFUNCTION(BlueprintCallable, Category="Phone")
	void RequestHangUpCall();

	/**
	 * 开关手机界面。**蓝图在这里实现**（把手机 Widget 加进/移出视口）。
	 *
	 * C++ 侧只负责按键：下面的 TogglePhoneKey 走 BindKey 直接绑，
	 * 不再依赖 IMC_Default 里的 J → IA_TogglePhone 映射。
	 * 那个资产从来没有被提交过，每次 git pull 都会被冲掉，而症状极具迷惑性——
	 * 手机面板本身的显示逻辑不依赖按键绑定，所以表现是"UI 突然坏了"
	 * 而不是"按键没绑"，很难往这个方向想。F 键已经因此丢过两次。
	 * 物品栏 1~5、切视角 P、召唤 R 都是同样理由直接绑的。
	 */
	UFUNCTION(BlueprintImplementableEvent, Category="Phone")
	void TogglePhoneUI();

	/** 开关手机的按键。改这里提示里的键名会跟着变，不会说一套做一套。 */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	FKey TogglePhoneKey;

	/**
	 * 报告某个 UI 面板开了还是关了。**鼠标光标和输入模式由这里统一算出来**：
	 * 只要还有任何面板开着就显示光标（`GameAndUI`），全关了就回到纯游戏（`GameOnly`）。
	 *
	 * **调用方一律不要自己写 `bShowMouseCursor`。** 那正是"骑车时鼠标转不了视角、
	 * 必须按住左键或右键"的根源：原来 `BP_DeliveryManPC` 在手机键上
	 * `Set bShowMouseCursor = NOT(Get bShowMouseCursor)`，和面板开关是**两个互不相干的
	 * 取反开关**——只要有一次只翻了其中一个（面板里点关闭、来电自动弹出、
	 * 按键被吃掉……），两者就永久失同步。光标留在开着的状态时鼠标移动归光标，
	 * 视口只在按住鼠标键时才捕获鼠标，于是鼠标看起来"失灵"了。
	 * 改成"光标是面板状态的函数"之后，这种失同步在结构上就不可能发生。
	 *
	 * 面板用 `PanelId` 区分（手机、任务面板、以后的商店等各用一个名字），
	 * 所以同时开两个面板、关掉其中一个时光标不会被错误地收走。
	 * **面板若有"自己关掉自己"的路径（关闭按钮、Destruct），那里也要调一次 false**，
	 * 否则光标会留着——这是本设计唯一需要调用方配合的地方。
	 */
	UFUNCTION(BlueprintCallable, Category="UI")
	void SetUIPanelOpen(FName PanelId, bool bOpen);

	/** 手机所有开关入口在实际改变面板后报告状态，包含来电自动展开和接听收起。 */
	UFUNCTION(BlueprintCallable, Category="Phone", meta=(DefaultToSelf="PhoneWidget"))
	static void ReportPhoneUIOpen(UUserWidget* PhoneWidget, bool bOpen);

	/** 当前是否还有 UI 面板开着（= 该不该显示光标）。 */
	UFUNCTION(BlueprintPure, Category="UI")
	bool IsAnyUIPanelOpen() const;

	/** 手机界面当前是开着的吗。状态由 C++ 持有，蓝图不要另存一份。 */
	UFUNCTION(BlueprintPure, Category="Phone")
	bool IsPhoneUIOpen() const { return bPhoneUIOpen; }

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION(Server, Reliable)
	void ServerAnswerCall();

	UFUNCTION(Server, Reliable)
	void ServerHangUpCall();


	/** Input Mapping Contexts always applied */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/** Extra mapping contexts for mouse look, skipped on touch devices */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> MobileExcludedMappingContexts;

	/**
	 * 交互键（F）。上面那些 IMC 资产里没有映射它时，运行时补一份。
	 *
	 * 为什么要兜底：F→IA_Interact 这条映射存在 IMC_Default.uasset 里，
	 * 而这个二进制资产被 git 拉取冲掉过两次。症状极具迷惑性——浮窗「按 F 驾驶」
	 * 照常显示（浮窗不依赖按键绑定），只有按下去没反应，很容易当成代码坏了去查编译。
	 * 补一份运行时映射之后，功能就不再取决于那个资产有没有被正确提交。
	 */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TSoftObjectPtr<UInputAction> InteractAction;

	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	FKey InteractKey;

	/** 本机交互物和抓取候选使用的透视高亮后处理；任务地点不再高亮。 */
	UPROPERTY(VisibleAnywhere, Category="Delivery|Highlight")
	TObjectPtr<UPostProcessComponent> HighlightPostProcess;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|Voice", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UDeliveryDialogueVoiceComponent> DialogueVoice;

	/** 背景音乐。轨道插槽填在 BP_DeliveryManPC 上，电话期间自动闪避。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Audio")
	TObjectPtr<UDeliveryMusicComponent> Music;

	/** 手机界面每次开/关时的按键反馈。需要区分开和关时可在 TogglePhoneUI 蓝图事件里另外播放。 */
	UPROPERTY(EditDefaultsOnly, Category="Delivery|Audio|Phone")
	TObjectPtr<USoundBase> PhoneToggleSound;

	/** 来电铃声。资产本身应设置循环；离开 Ringing 状态时 C++ 会主动停止。 */
	UPROPERTY(EditDefaultsOnly, Category="Delivery|Audio|Phone")
	TObjectPtr<USoundBase> IncomingCallSound;

	UPROPERTY(EditDefaultsOnly, Category="Delivery|Audio|Phone")
	TObjectPtr<USoundBase> AnswerCallSound;

	UPROPERTY(EditDefaultsOnly, Category="Delivery|Audio|Phone")
	TObjectPtr<USoundBase> HangUpCallSound;

	virtual void SetupInputComponent() override;

	/** TogglePhoneKey 按下时的转发。BindKey 需要一个无参成员函数，不能直接绑 BlueprintImplementableEvent。 */
	void HandleTogglePhonePressed();

	/** 按当前开着的面板集合算出光标和输入模式。只在本机控制器上生效。 */
	void ApplyUIInputMode();

private:

	/**
	 * 当前开着的 UI 面板。只在本机维护、不复制。
	 *
	 * 用集合而不是一个 bool：同时开手机和任务面板时，关掉一个不该把光标收走。
	 */
	TSet<FName> OpenUIPanels;

	/** 手机界面开没开。C++ 持有，这样光标状态不可能和它脱钩。 */
	bool bPhoneUIOpen = false;

	/** GameState 可能比本机 PlayerController 晚就绪，绑定不到时短暂重试。 */
	void BindPhoneVoice();

	/** 电话进入 InCall 时在本机播放任务台词；离开通话立即停声。 */
	UFUNCTION()
	void HandlePhoneVoiceStateChanged(EDeliveryPhoneCallState NewState, const FDeliveryPhoneCall& Call);

	/** 程序化语音每揭示一个字符，就把它追加到屏幕下方字幕。 */
	UFUNCTION()
	void HandlePhoneVoiceGlyphRevealed(const FString& Glyph, int32 GlyphIndex);

	/** 已有 IMC 里都没映射交互键时，建一份临时的补上。 */
	void EnsureInteractMapping(class UEnhancedInputLocalPlayerSubsystem* Subsystem);

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> RuntimeInteractContext;

	FTimerHandle PhoneVoiceBindRetryTimer;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> IncomingCallAudio;

	void StopIncomingCallAudio();
	bool bPhoneAudioStateInitialized = false;
	EDeliveryPhoneCallState LastPhoneAudioState = EDeliveryPhoneCallState::Idle;
	int32 ActiveVoiceCallId = 0;
	FText ActivePhoneCallerName;
	FString ActivePhoneSubtitle;
};
