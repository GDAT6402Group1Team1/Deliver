#include "DeliveryResidentToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "SDeliveryPoseDebug.h"
#include "Modules/ModuleManager.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/Blueprint.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "ToolMenus.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace DeliveryTuning
{
const FName TabId(TEXT("DeliveryTuning"));

// 只编辑用户明确选择的关卡实例；蓝图默认值交给原生蓝图编辑器处理
class SPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SPanel) {}
    SLATE_END_ARGS()

    // 创建出拳、布娃娃参数页和调参说明
    void Construct(const FArguments&)
    {
        FDetailsViewArgs Args;
        Args.bAllowSearch = true;
        Args.bHideSelectionTip = true;
        Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
        FPropertyEditorModule& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
        CombatDetails = Properties.CreateDetailView(Args);
        RagdollDetails = Properties.CreateDetailView(Args);
        // 运行期间禁用参数修改，避免把临时游戏状态当成关卡配置
        const auto CanEdit = FIsPropertyEditingEnabled::CreateLambda([] { return GEditor && !GEditor->PlayWorld; });
        CombatDetails->SetIsPropertyEditingEnabledDelegate(CanEdit);
        RagdollDetails->SetIsPropertyEditingEnabledDelegate(CanEdit);

        TSharedRef<SScrollBox> Guide = SNew(SScrollBox);
        LoadGuide(Guide);
        ChildSlot
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(8)
            [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("停止运行后调参，改完重新运行查看效果。"))) ]
            + SVerticalBox::Slot().AutoHeight().Padding(8, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(FText::FromString(TEXT("读取选中角色"))).OnClicked(this, &SPanel::ReadSelection) ]
                + SHorizontalBox::Slot().AutoWidth()
                [ SNew(SButton).Text(FText::FromString(TEXT("角色蓝图"))).OnClicked(this, &SPanel::OpenBlueprint) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(8)
            [ SAssignNew(Status, STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("选中关卡角色后读取；修改默认参数请打开角色蓝图。"))) ]
            + SVerticalBox::Slot().FillHeight(1)
            [
                SNew(SSplitter)
                + SSplitter::Slot().Value(.28f) [ Guide ]
                + SSplitter::Slot().Value(.72f)
                [ SNew(SVerticalBox)
                  + SVerticalBox::Slot().AutoHeight().Padding(6)
                  [ SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("出拳")))
                      .ButtonColorAndOpacity_Lambda([this] { return SectionIndex == 0 ? FLinearColor(.15f, .4f, .7f) : FLinearColor(.2f, .2f, .2f); })
                      .OnClicked_Lambda([this] { PoseDebug->StopDiagnostics(); SectionIndex = 0; return FReply::Handled(); }) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(FText::FromString(TEXT("布娃娃")))
                      .ButtonColorAndOpacity_Lambda([this] { return SectionIndex == 1 ? FLinearColor(.15f, .4f, .7f) : FLinearColor(.2f, .2f, .2f); })
                      .OnClicked_Lambda([this] { PoseDebug->StopDiagnostics(); SectionIndex = 1; return FReply::Handled(); }) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("实时诊断")))
                      .OnClicked_Lambda([this] { SectionIndex = 2; return FReply::Handled(); }) ]
                  ]
                  + SVerticalBox::Slot().FillHeight(1)
                  [ SNew(SWidgetSwitcher).WidgetIndex_Lambda([this] { return SectionIndex; })
                    + SWidgetSwitcher::Slot() [ CombatDetails.ToSharedRef() ]
                    + SWidgetSwitcher::Slot() [ RagdollDetails.ToSharedRef() ]
                    + SWidgetSwitcher::Slot() [ SAssignNew(PoseDebug, SDeliveryPoseDebug) ]
                  ]
                ]
            ]
        ];
    }

private:
    // 读取中文调参说明，按分类生成可展开的说明栏
    void LoadGuide(const TSharedRef<SScrollBox>& Guide)
    {
        FString Text;
        TArray<TSharedPtr<FJsonValue>> Sections;
        const FString Path = FPaths::ProjectDir() / TEXT("Document/RagdollTuning.json");
        if (!FFileHelper::LoadFileToString(Text, *Path) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Sections))
        {
            Guide->AddSlot()[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("未能读取 Document/RagdollTuning.json，请检查文件。")))];
            return;
        }
        for (const TSharedPtr<FJsonValue>& Value : Sections)
        {
            const TSharedPtr<FJsonObject> Section = Value->AsObject();
            FString Title, Body;
            if (!Section.IsValid() || !Section->TryGetStringField(TEXT("title"), Title) || !Section->TryGetStringField(TEXT("body"), Body))
                continue;
            Guide->AddSlot().Padding(8)
            [ SNew(SExpandableArea).InitiallyCollapsed(true)
              .HeaderContent()[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Title))]
              .BodyContent()[SNew(SBorder).Padding(10)[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Body))]]
            ];
        }
    }

    // 读取关卡中选中的角色组件，供详情面板直接编辑
    FReply ReadSelection()
    {
        if (!GEditor || GEditor->PlayWorld)
        {
            Status->SetText(FText::FromString(TEXT("请先停止运行，再读取角色。")));
            return FReply::Handled();
        }
        // 先清空旧对象，选中项无效时不能继续修改上一次的角色
        CombatDetails->SetObject(nullptr);
        RagdollDetails->SetObject(nullptr);
        AActor* Selected = nullptr;
        int32 Matches = 0;
        for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
        {
            AActor* Actor = Cast<AActor>(*It);
            if (Actor && Actor->FindComponentByClass<UDeliveryActiveRagdollComponent>())
            {
                Selected = Actor;
                ++Matches;
            }
        }
        if (Matches != 1)
        {
            Status->SetText(FText::FromString(TEXT("请选择一个 Delivery 角色，或打开角色蓝图。")));
            return FReply::Handled();
        }
        // 面板直接编辑选中实例的组件，修改随关卡保存
        CombatDetails->SetObject(Selected->FindComponentByClass<UDeliveryRagdollCombatComponent>());
        RagdollDetails->SetObject(Selected->FindComponentByClass<UDeliveryActiveRagdollComponent>());
        Status->SetText(FText::FromString(FString::Printf(TEXT("当前角色：%s｜保存地图保留修改，Ctrl+Z 撤销。"), *Selected->GetActorLabel())));
        return FReply::Handled();
    }

    // 打开角色蓝图，供修改所有实例使用的默认参数
    FReply OpenBlueprint()
    {
        if (!GEditor || GEditor->PlayWorld)
        {
            Status->SetText(FText::FromString(TEXT("请先停止 PIE，再编辑蓝图默认参数。")));
            return FReply::Handled();
        }
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan"));
        if (Blueprint)
        {
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Blueprint);
            Status->SetText(FText::FromString(TEXT("选择 RagdollCombat 或 ActiveRagdoll，改完编译并保存蓝图。")));
        }
        else
            Status->SetText(FText::FromString(TEXT("角色蓝图路径不存在，请从内容浏览器打开实际使用的角色蓝图。")));
        return FReply::Handled();
    }

    // 零表示出拳页，一表示布娃娃页，二表示实时诊断
    TSharedPtr<SDeliveryPoseDebug> PoseDebug;
    int32 SectionIndex = 0;
    TSharedPtr<IDetailsView> CombatDetails;
    TSharedPtr<IDetailsView> RagdollDetails;
    TSharedPtr<STextBlock> Status;
};
}

class FDeliveryTuningModule : public IModuleInterface
{
public:
    // 注册调参窗口，并在编辑器菜单就绪后添加入口
    void StartupModule() override
    {
        UToolsetRegistry::RegisterToolsetClass(UDeliveryResidentToolset::StaticClass());
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(DeliveryTuning::TabId,
            FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) {
                return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(DeliveryTuning::SPanel)];
            })).SetDisplayName(FText::FromString(TEXT("Delivery 布娃娃与出拳调参")));
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FDeliveryTuningModule::RegisterMenus));
    }

    // 卸载模块时移除窗口和菜单回调
    void ShutdownModule() override
    {
        UToolsetRegistry::UnregisterToolsetClass(UDeliveryResidentToolset::StaticClass());
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(DeliveryTuning::TabId);
    }

private:
    // 在窗口菜单添加调参工具入口
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
        Menu->FindOrAddSection("Delivery").AddMenuEntry("Delivery.Tuning",
            FText::FromString(TEXT("Delivery 布娃娃与出拳调参")),
            FText::FromString(TEXT("打开中文调参教程和角色组件参数。")), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(DeliveryTuning::TabId); })));
    }
};

IMPLEMENT_MODULE(FDeliveryTuningModule, DeliveryTuning)
