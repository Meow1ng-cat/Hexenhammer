// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Hexenhammer : ModuleRules
{
	public Hexenhammer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
            "OnlineSubsystem",
			"OnlineSubsystemUtils"
        });

		// ControlRig and RigVM for the blade guard, a rig unit that runs inside the animation evaluation.
		PrivateDependencyModuleNames.AddRange(new string[] { "ControlRig", "RigVM" });

		PublicIncludePaths.AddRange(new string[] {
			"Hexenhammer",
			"Hexenhammer/Variant_Platforming",
			"Hexenhammer/Variant_Platforming/Animation",
			"Hexenhammer/Variant_Combat",
			"Hexenhammer/Variant_Combat/AI",
			"Hexenhammer/Variant_Combat/Animation",
			"Hexenhammer/Variant_Combat/Gameplay",
			"Hexenhammer/Variant_Combat/Interfaces",
			"Hexenhammer/Variant_Combat/UI",
			"Hexenhammer/Variant_SideScrolling",
			"Hexenhammer/Variant_SideScrolling/AI",
			"Hexenhammer/Variant_SideScrolling/Gameplay",
			"Hexenhammer/Variant_SideScrolling/Interfaces",
			"Hexenhammer/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
