// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UtilityShaders : ModuleRules
{
	public UtilityShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"RHI",
                "RenderCore",
			}
		);
	}
}
