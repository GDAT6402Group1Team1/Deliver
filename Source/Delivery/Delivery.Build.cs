// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class Delivery : ModuleRules
{
	public Delivery(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule",
			"SmartObjectsModule",
			"AudioMixer",
			"InputCore",
			"EnhancedInput",
			"PhysicsControl",
			"GeometryCollectionEngine",
            "GameplayAbilities",
            "GameplayTags",
            "GameplayTasks",
            // 交互浮窗直接用 Slate 画（见 Interaction/DeliveryPromptSubsystem），
            // UMG 只用来做世界坐标 → 屏幕坐标的换算（那一步自带 DPI 缩放处理）。
            "Slate",
            "SlateCore",
            "UMG",
            // 地图朝向那一个设置走 UDeveloperSettings（见 DeliveryMapSettings.h）
            "DeveloperSettings"
        });

		PrivateDependencyModuleNames.AddRange(new string[] { "PhysicsCore", "Chaos", "HTTP", "Json", "Voice", "NavigationSystem" });

        PrivateDependencyModuleNames.Add("AudioCaptureCore");
        AddEngineThirdPartyPrivateStaticDependencies(Target, "WebRTC");
        string ProjectRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "../.."));
        PrivateIncludePaths.Add(Path.Combine(ProjectRoot, "Tools", "DeliverySpeech", "native"));
        // 模型和 DLL 按原目录读取，运行时只加载已经部署的资源，不下载或写回内容目录
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string SpeechRuntime = Path.Combine(ProjectRoot, "Binaries", "ThirdParty", "DeliverySpeech");
            foreach (string FileName in new[] { "DeliverySpeechRuntime.dll", "build.json", "runtime_manifest.json" })
                RuntimeDependencies.Add(Path.Combine(SpeechRuntime, FileName), StagedFileType.NonUFS);
            foreach (string ModelName in new[] { "encoder.int8.onnx", "decoder.onnx", "joiner.int8.onnx", "tokens.txt" })
                RuntimeDependencies.Add(Path.Combine(SpeechRuntime, "models", ModelName), StagedFileType.NonUFS);
            RuntimeDependencies.Add(Path.Combine(SpeechRuntime, "licenses", "*.txt"), StagedFileType.NonUFS);
        }
		// 真实启动地图的 PIE 回归只在编辑器目标中编译
		if (Target.bBuildEditor) PrivateDependencyModuleNames.Add("UnrealEd");
	}
}
