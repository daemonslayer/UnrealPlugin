// Copyright 2017 Fove, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class FoveHMD : ModuleRules
{
#if WITH_FORWARDED_MODULE_RULES_CTOR // Engine versions >= 4.16
    public FoveHMD(ReadOnlyTargetRules Target) : base(Target)
#else // Engine versions < 4.16
    public FoveHMD(TargetInfo Target)
#endif
    {
        string EnginePath = Path.GetFullPath(BuildConfiguration.RelativeEnginePath);
        string SourceRuntimePath = EnginePath + "Source/Runtime/";

        PrivateIncludePaths.AddRange(new string[] {
            "FoveHMD/Private",
            SourceRuntimePath + "Renderer/Private", // Needed for FRenderingCompositePassContext
        });

        // Add our public dependencies. Anything using FoveHMD will automatically get these
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "FoveVR"
        });

        // Add our private dependencies. Anything we need internally to compile & link
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "HeadMountedDisplay",
            "Projects",
            "RHI",
            "RenderCore",
            "Renderer",
            "ShaderCore",
            "Slate",
            "SlateCore"
        });

        // Depend on the editor module if in the editor
        if (UEBuildConfiguration.bBuildEditor == true)
            PrivateDependencyModuleNames.Add("UnrealEd");

        // On windows, we use the D3D11 rendering interface
        if (Target.Platform == UnrealTargetPlatform.Win64)
             PrivateDependencyModuleNames.Add("D3D11RHI");
    }
}
