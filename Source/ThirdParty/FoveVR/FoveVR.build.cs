// Copyright 2017 Fove, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class FoveVR : ModuleRules
{
#if WITH_FORWARDED_MODULE_RULES_CTOR // Engine versions >= 4.16
    public FoveVR(ReadOnlyTargetRules Target) : base(Target)
#else // Engine versions < 4.16
    public FoveVR(TargetInfo Target)
#endif
    {
        Type = ModuleType.External;

        // Compute various needed paths
        string FoveSDKVersion = "0_13_0";
        string SDKVersionPath = "FoveVR_SDK_" + FoveSDKVersion;
        string BaseDirectory = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", ".."));
        string BinariesDir = Path.Combine(BaseDirectory, "Binaries", "ThirdParty", "FoveVR", SDKVersionPath);
        string SdkBase = Path.GetFullPath(Path.Combine(ModuleDirectory, SDKVersionPath));

        // Add the fove include directory
        string IncludePath = Path.Combine(SdkBase, "include");
        PublicIncludePaths.Add(IncludePath);

        // Add a search path to find the FoveClient dynamic library
        string LibraryArchPath = Path.Combine(BinariesDir, "x64");
        PublicLibraryPaths.Add(LibraryArchPath);

        // Link the correct fove library
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicAdditionalLibraries.Add("FoveClient.lib");
            PublicDelayLoadDLLs.Add("FoveClient.dll");
            RuntimeDependencies.Add(new RuntimeDependency(Path.Combine(BinariesDir, "x64", "FoveClient.dll")));
        }
    }
}
