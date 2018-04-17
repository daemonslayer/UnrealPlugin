#include "FoveHMD.h"
#include "FoveHMDPrivatePCH.h"
#include "Core.h"
#include "Engine.h"
#include "FoveVRFunctionLibrary.h"
#include "IFVRCompositor.h"
#include "IFVRHeadset.h"
#include "IFoveHMDPlugin.h"
#include "IPluginManager.h"
#include "PostProcessParameters.h"
#include "PostProcess/PostProcessHMD.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneViewport.h"

// Compile time developer option to change what mode you want to run in
enum class FoveUnrealPluginMode
{
	PositionAndOrientation, // The game will enable position and orientation tracking (if possible) and Unreal cameras will move/rotate with the users head
	OrientationOnly,        // The game will only enable orientation tracking, the position tracking camera will not be used
	FixedToHMDScreen,       // The rendered results of the game will not rotate/move with the head, and rendered content will be "fixed" to the HMD screen
};

// Developers can change this mode to change the behavior of the fove plugin at runtime
constexpr FoveUnrealPluginMode FoveMode = FoveUnrealPluginMode::PositionAndOrientation;

// Define or include the LogHMD category, depending on whether we are in Unreal 4.17+ or not
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 17
#include "LogCategory.h"
#else
DEFINE_LOG_CATEGORY_STATIC(LogHMD, Log, All);
#endif

// 4.16+ uses PipelineStateCache
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 16
#define FOVE_USE_PIPLINE_STATE_CACHE
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#endif

#if WITH_EDITOR
#include "Editor/UnrealEd/Classes/Editor/EditorEngine.h"
#endif

#if PLATFORM_WINDOWS
#include "AllowWindowsPlatformTypes.h"
#include <d3d11.h>
#include "HideWindowsPlatformTypes.h"
#endif // PLATFORM_WINDOWS

#define LOCTEXT_NAMESPACE "FFoveHMD"

//---------------------------------------------------
// Helpers
//---------------------------------------------------

#ifdef _MSC_VER
#pragma region Helpers
#else
#pragma mark Helpers
#endif

FMatrix ToUnreal(const Fove::SFVR_Matrix44& tm)
{
	return FMatrix(
		FPlane(tm.mat[0][0], tm.mat[1][0], tm.mat[2][0], tm.mat[3][0]),
		FPlane(tm.mat[0][1], tm.mat[1][1], tm.mat[2][1], tm.mat[3][1]),
		FPlane(tm.mat[0][2], tm.mat[1][2], tm.mat[2][2], tm.mat[3][2]),
		FPlane(tm.mat[0][3], tm.mat[1][3], tm.mat[2][3], tm.mat[3][3]));
}

FQuat ToUnreal(const Fove::SFVR_Quaternion quat)
{
	return FQuat(quat.z, quat.x, quat.y, quat.w);
}

FVector ToUnreal(const Fove::SFVR_Vec3 vec, const float scale)
{
	return FVector(vec.z * scale, vec.x * scale, vec.y * scale);
}

FTransform ToUnreal(const Fove::SFVR_Pose& pose, const float scale)
{
	FQuat FoveOrientation = ToUnreal(pose.orientation);
	FVector FovePosition = ToUnreal(pose.position, scale);
	return FTransform(FoveOrientation, FovePosition);
}

// Helper function for acquiring the appropriate FSceneViewport
FSceneViewport* FoveFindSceneViewport()
{
	if (!GIsEditor)
	{
		UGameEngine* const GameEngine = Cast<UGameEngine>(GEngine);
		return GameEngine->SceneViewport.Get();
	}
#if WITH_EDITOR
	else
	{
		UEditorEngine* const EditorEngine = CastChecked<UEditorEngine>(GEngine);
		FSceneViewport* const PIEViewport = (FSceneViewport*)EditorEngine->GetPIEViewport();
		if (PIEViewport != nullptr && PIEViewport->IsStereoRenderingAllowed())
		{
			// PIE is setup for stereo rendering
			return PIEViewport;
		}
		else
		{
			// Check to see if the active editor viewport is drawing in stereo mode
			FSceneViewport* EditorViewport = (FSceneViewport*)EditorEngine->GetActiveViewport();
			if (EditorViewport != nullptr && EditorViewport->IsStereoRenderingAllowed())
			{
				return EditorViewport;
			}
		}
	}
#endif
	return nullptr;
}

// Helper function to determine if a fove is connected
bool IsFoveConnected(Fove::IFVRHeadset& headset, Fove::IFVRCompositor& compositor)
{
	// Headset must be plugged in
	bool isHardwareConnected = false;
	Fove::EFVR_ErrorCode Error = headset.IsHardwareConnected(&isHardwareConnected);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::IsHardwareConnected failed: %d"), static_cast<int>(Error));
	if (!isHardwareConnected)
		return false;

	// Check if we are connected to the compositor
	// This is an important step because there are potentially other Unreal plugins that support FOVE (such as SteamVR and OSVR)
	// In all cases, the FOVE headset may be connected, but we should only use the FOVE plugin when the FOVE compositor is running
	bool isCompositorReady = false;
	Error = compositor.IsReady(&isCompositorReady);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRCompositor::IsReady failed: %d"), static_cast<int>(Error));
	if (!isCompositorReady)
		return false;
	
	return true;
}

#ifdef _MSC_VER
#pragma endregion
#endif

//---------------------------------------------------
// FoveRenderingBridge
//---------------------------------------------------

#ifdef _MSC_VER
#pragma region FoveRenderingBridge
#else
#pragma mark FoveRenderingBridge
#endif

class FoveRenderingBridge : public FRHICustomPresent
{
public:
	FoveRenderingBridge(const TSharedRef<Fove::IFVRCompositor, ESPMode::ThreadSafe>& compositor) : FRHICustomPresent(nullptr), Compositor(compositor) {}
	virtual ~FoveRenderingBridge() {}

	void OnBackBufferResize() override {} // Ignored

	const void SetRenderPose(const Fove::SFVR_Pose& pose, const float WorldToMetersScale)
	{
		FovePose = pose;
		Pose = ToUnreal(FovePose, WorldToMetersScale);
	}

	const FTransform& GetRenderPose() const
	{
		return Pose;
	}

	virtual void UpdateViewport(const FViewport& Viewport) = 0;

protected:
	const TSharedRef<Fove::IFVRCompositor, ESPMode::ThreadSafe> Compositor;  // Pointer back to the Fove plugin object that owns us
	Fove::SFVR_Pose FovePose;  // Pose fetched out via WaitForRenderPose, used internally to submit frames back to fove
	FTransform Pose;           // Same as RenderPose, but converted to Unreal coordinates
};

#ifdef _MSC_VER
#pragma endregion
#endif

//---------------------------------------------------
// FoveD3D11Bridge
//---------------------------------------------------

#ifdef _MSC_VER
#pragma region FoveD3D11Bridge
#else
#pragma mark FoveD3D11Bridge
#endif

#if PLATFORM_WINDOWS

class FoveD3D11Bridge : public FoveRenderingBridge
{
	ID3D11Texture2D* RenderTargetTexture = nullptr;
	const Fove::SFVR_CompositorLayer FoveCompositorLayer;

public:
	FoveD3D11Bridge(const TSharedRef<Fove::IFVRCompositor, ESPMode::ThreadSafe>& Compositor, Fove::SFVR_CompositorLayer Layer)
		: FoveRenderingBridge(Compositor)
		, FoveCompositorLayer(Layer)
	{
	}

	~FoveD3D11Bridge()
	{
		if (RenderTargetTexture)
			RenderTargetTexture->Release();
	}

	bool Present(int& SyncInterval) override
	{
		check(IsInRenderingThread());

		if (!RenderTargetTexture)
		{
			UE_LOG(LogHMD, Warning, TEXT("FOVE present without render texture"));
			return false;
		}

		// Clear rasterizer state to avoid Unreal messing with FOVE submit
		ID3D11Device* Dev = nullptr;
		ID3D11DeviceContext* Ctx = nullptr;
		ID3D11RasterizerState* RasterizerState = nullptr;
		RenderTargetTexture->GetDevice(&Dev);
		if (Dev)
		{
			Dev->GetImmediateContext(&Ctx);
			if (Ctx)
			{
				D3D11_RASTERIZER_DESC desc = {};
				Ctx->RSGetState(&RasterizerState);
				Ctx->RSSetState(nullptr);
			}
		}

		// Submit eye images
		Fove::SFVR_CompositorLayerSubmitInfo info;
		info.layerId = FoveCompositorLayer.layerId;
		info.pose = FovePose;
		info.left.texInfo = RenderTargetTexture;
		info.right.texInfo = RenderTargetTexture;
		info.left.bounds.left = 0.0f;
		info.left.bounds.right = 0.5f;
		info.left.bounds.bottom = 1.0f;
		info.left.bounds.top = 0.0f;
		info.right.bounds.left = 0.5f;
		info.right.bounds.right = 1.0f;
		info.right.bounds.bottom = 1.0f;
		info.right.bounds.top = 0.0f;
		Compositor->Submit(info);

		// Restore state
		if (Ctx)
		{
			D3D11_RASTERIZER_DESC desc = {};
			Ctx->RSSetState(RasterizerState);
		}

		return true;
	}

	void UpdateViewport(const FViewport& Viewport) override
	{
		check(IsInGameThread());

		// Update render target
		const FTexture2DRHIRef& textureRef = Viewport.GetRenderTargetTexture();
		ID3D11Texture2D* const newRT = textureRef ? (ID3D11Texture2D*)textureRef->GetNativeResource() : nullptr;
		if (newRT != RenderTargetTexture)
		{
			if (RenderTargetTexture)
				RenderTargetTexture->Release();

			RenderTargetTexture = newRT;
			
			if (RenderTargetTexture)
				RenderTargetTexture->AddRef();
		}
	}

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
	bool NeedsNativePresent() override
	{
		return true;
	}
#endif
};

#endif // PLATFORM_WINDOWS

#ifdef _MSC_VER
#pragma endregion
#endif

//---------------------------------------------------
// UFoveVRFunctionLibrary
//---------------------------------------------------

#ifdef _MSC_VER
#pragma region UFoveVRFunctionLibrary
#else
#pragma mark UFoveVRFunctionLibrary
#endif

UFoveVRFunctionLibrary::UFoveVRFunctionLibrary(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UFoveVRFunctionLibrary::IsHardwareConnected()
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
		return hmd->IsHardwareConnected();

	return false;
}

bool UFoveVRFunctionLibrary::IsHardwareReady()
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
		return hmd->IsHardwareReady();

	return false;
}

bool UFoveVRFunctionLibrary::IsEyeTrackingCalibrating()
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
	{
		hmd->GetHeadset().EnsureEyeTrackingCalibration();
		return true;
	}

	return false;
}

bool UFoveVRFunctionLibrary::EnsureEyeTrackingCalibration()
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
		return hmd->EnsureEyeTrackingCalibration();

	return false;
}

bool UFoveVRFunctionLibrary::GetGazeConvergence(const bool bRelativeToHMD, FVector& outRayOrigin, FVector& outRayDirection, float& outDistance, float& outAccuracy)
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
		return hmd->GetGazeConvergence(bRelativeToHMD, &outRayOrigin, &outRayDirection, &outDistance, &outAccuracy);

	return false;
}

bool UFoveVRFunctionLibrary::GetGazeVector(const bool bRelativeToHMD, FVector& outLeft, FVector& outRight)
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
		return hmd->GetGazeVector(bRelativeToHMD, &outLeft, &outRight);

	return false;
}

bool UFoveVRFunctionLibrary::GetGazeVector2D(FVector2D& outLeft, FVector2D& outRight)
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
		return hmd->GetGazeVector2D(&outLeft, &outRight);

	return false;
}

bool UFoveVRFunctionLibrary::ManualDriftCorrection3D(const FVector Location)
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
	{
		hmd->ManualDriftCorrection3D(Location);
		return true;
	}

	return false;
}

bool UFoveVRFunctionLibrary::CheckEyesTracked(bool& outLeft, bool& outRight)
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
		return hmd->CheckEyesTracked(&outLeft, &outRight);

	return false;
}

bool UFoveVRFunctionLibrary::CheckEyesClosed(bool& outLeft, bool& outRight)
{
	if (FFoveHMD* const hmd = FFoveHMD::Get())
		return hmd->CheckEyesClosed(&outLeft, &outRight);

	return false;
}

bool UFoveVRFunctionLibrary::IsPositionReady()
{
	bool Ret = false;

	if (FFoveHMD* const hmd = FFoveHMD::Get())
	{
		const Fove::EFVR_ErrorCode Error = hmd->GetHeadset().IsPositionReady(&Ret);
		if (Error != Fove::EFVR_ErrorCode::None)
			UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::IsPositionReady failed: %d"), static_cast<int>(Error));
	}

	return Ret;
}

#ifdef _MSC_VER
#pragma endregion
#endif

//---------------------------------------------------
// FFoveHMDPlugin
//---------------------------------------------------

#ifdef _MSC_VER
#pragma region FFoveHMDPlugin
#else
#pragma mark FFoveHMDPlugin
#endif

class FFoveHMDPlugin : public IFoveHMDPlugin
{
public: // IHeadMountedDisplayModule implementation

	void StartupModule() override
	{
		IFoveHMDPlugin::StartupModule();

		// On windows, we delay loading of the DLL, so the game can function if it's missing
		// This is not implemented on other platforms currently
#if PLATFORM_WINDOWS
		if (!dllHandle)
		{
			// Get the library path based on the base dir of this plugin
			const FString baseDir = IPluginManager::Get().FindPlugin("FoveHMD")->GetBaseDir();
			const FString foveLibDir = FString::Printf(TEXT("Binaries/ThirdParty/FoveVR/FoveVR_SDK_%s/x64/FoveClient.dll"), FOVEVR_SDK_VER);
			const FString libraryPath = FPaths::Combine(*baseDir, *foveLibDir);

			// Load the fove client dll and show an error if it fails
			dllHandle = !libraryPath.IsEmpty() ? FPlatformProcess::GetDllHandle(*libraryPath) : nullptr;
			if (!dllHandle)
			{
				UE_LOG(LogHMD, Warning, TEXT("Failed to load FoveVR DLL handle"));
				FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to load FoveClient: " + libraryPath));
				return;
			}
		}
#endif

		// We do not create the Headset and Compositor objects here, hence the CreateObjectsIfNeeded() function
		// If we do so, it causes a "SECURE CRT: Invalid parameter detected" error when packing projects with the FOVE plugin
		// The reason for this is unknown
	}

	void ShutdownModule() override
	{
		// Clear headset & compositor
		// It is assumed that all other references are cleared by now as well
		Headset.Reset();
		Compositor.Reset();

		// Unload the fove client dll
		if (dllHandle)
		{
			FPlatformProcess::FreeDllHandle(dllHandle);
			dllHandle = nullptr;
		}

		// Call base class last per standard ordering
		IFoveHMDPlugin::ShutdownModule();
	}


#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
	TSharedPtr<class IXRTrackingSystem  , ESPMode::ThreadSafe> CreateTrackingSystem() override
#else
	TSharedPtr<class IHeadMountedDisplay, ESPMode::ThreadSafe> CreateHeadMountedDisplay() override
#endif
	{
		CreateObjectsIfNeeded();
		if (!Headset.IsValid() || !Compositor.IsValid())
			return nullptr;

		// Create a compositor layer
		Fove::SFVR_CompositorLayer Layer;
		Fove::SFVR_CompositorLayerCreateInfo LayerCreateInfo;
		LayerCreateInfo.disableTimeWarp = FoveMode == FoveUnrealPluginMode::FixedToHMDScreen;
		Compositor->CreateLayer(LayerCreateInfo, &Layer);

		TSharedPtr<FFoveHMD, ESPMode::ThreadSafe> FoveHMD(new FFoveHMD(Headset.ToSharedRef(), MoveTemp(Compositor), Layer));

		// Compositor should be moved into the FFoveHMD class, but clear it just in cas
		// This ensures that, if we create antoher FFoveHMD, it will get it's own compositor with it's own layer
		// The old FFoveHMD will destroy it's own compositor (and layer) when it dies
		// Currently there is no destroy layer functionality so we must destroy the IFVRCompositor object itself
		Compositor = TUniquePtr<Fove::IFVRCompositor>();

		return FoveHMD;
	}

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 14
	FString GetModuleKeyName() const override
#else
	FString GetModulePriorityKeyName() const override
#endif
	{
		return FString(TEXT("FoveHMD"));
	}
	
	bool IsHMDConnected() override
	{
		check(IsInGameThread());

		CreateObjectsIfNeeded();
		return Headset.IsValid() && Compositor.IsValid() && IsFoveConnected(*Headset, *Compositor);
	}

private:

	void CreateObjectsIfNeeded()
	{
		if (!Headset.IsValid())
		{
			// Create the headset object
			Headset = TSharedPtr<Fove::IFVRHeadset, ESPMode::ThreadSafe>(Fove::GetFVRHeadset());
			if (!Headset.IsValid())
			{
				UE_LOG(LogHMD, Warning, TEXT("Failed to create IFVRHeadset"));
				return;
			}

			// Determine what FOVE capabilities we want to enable
			Fove::EFVR_ClientCapabilities Capabilities = Fove::EFVR_ClientCapabilities::Gaze; // Change Gaze to None to disable gaze tracking
			if (FoveMode == FoveUnrealPluginMode::PositionAndOrientation)
				Capabilities = Capabilities | Fove::EFVR_ClientCapabilities::Position;
			if (FoveMode == FoveUnrealPluginMode::PositionAndOrientation || FoveMode == FoveUnrealPluginMode::OrientationOnly)
				Capabilities = Capabilities | Fove::EFVR_ClientCapabilities::Orientation;

			// Initialize headset
			Headset->Initialise(Capabilities);
		}

		if (!Compositor.IsValid())
		{
			// Create or destroy the compositor object as needed
			// To lower overhead and not open IPC to the compositor, we do this only once the headset is plugged in
			Compositor = TUniquePtr<Fove::IFVRCompositor>(Fove::GetFVRCompositor());
			if (!Compositor.IsValid())
			{
				UE_LOG(LogHMD, Warning, TEXT("Failed to create IFVRCompositor"));
				return;
			}
		}
	}

	// Headset and compositor objects, these are shared with the FFoveHMD devices that we create
	TSharedPtr<Fove::IFVRHeadset, ESPMode::ThreadSafe> Headset;
	TUniquePtr<Fove::IFVRCompositor> Compositor;

	void* dllHandle = nullptr;
};

IMPLEMENT_MODULE(FFoveHMDPlugin, FoveHMD)

#ifdef _MSC_VER
#pragma endregion
#endif

//---------------------------------------------------
// FFoveHMD
//---------------------------------------------------

#ifdef _MSC_VER
#pragma region FFoveHMD
#else
#pragma mark FFoveHMD
#endif

FFoveHMD::FFoveHMD(TSharedRef<Fove::IFVRHeadset, ESPMode::ThreadSafe> headset, TUniquePtr<Fove::IFVRCompositor> compositor, Fove::SFVR_CompositorLayer layer)
	: ZNear(GNearClippingPlane)
	, ZFar(GNearClippingPlane)
	, FoveHeadset(MoveTemp(headset))
	, FoveCompositor(compositor.Release())
	, FoveCompositorLayer(layer)
	, Bridge(*(new TRefCountPtr<FoveRenderingBridge>))
{
	IHeadMountedDisplay::StartupModule();

	// Grab a pointer to the renderer module
	static const FName RendererModuleName("Renderer");
	RendererModule = FModuleManager::GetModulePtr<IRendererModule>(RendererModuleName);

#if PLATFORM_WINDOWS
	if (IsPCPlatform(GMaxRHIShaderPlatform) && !IsOpenGLPlatform(GMaxRHIShaderPlatform))
	{
		Bridge = TRefCountPtr<FoveRenderingBridge>(new FoveD3D11Bridge(FoveCompositor, FoveCompositorLayer));
	}
#endif

	UE_LOG(LogHMD, Log, TEXT("FFoveHMD initialized"));
}

FFoveHMD::~FFoveHMD()
{
	UE_LOG(LogHMD, Log, TEXT("FFoveHMD destructing"));

	delete &Bridge;
}

FFoveHMD* FFoveHMD::Get()
{
	// Get the global HMD object
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
	IHeadMountedDisplay* const Hmd = GEngine->XRSystem->GetHMDDevice();
#else
	IHeadMountedDisplay* const Hmd = GEngine->HMDDevice.Get();
#endif
	if (!Hmd)
		return nullptr;

	// Check if the HMD object is a FoveHMD device
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
	if (GEngine->XRSystem->GetSystemName() != TEXT("FoveHMD"))
#elif ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 13
	if (Hmd->GetDeviceName() != TEXT("FoveHMD"))
#else
	if (Hmd->GetHMDDeviceType() != EHMDDeviceType::DT_ES2GenericStereoMesh)
#endif
		return nullptr;

	return static_cast<FFoveHMD*>(Hmd);
}

bool FFoveHMD::IsHardwareConnected() const
{
	bool Ret = false;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->IsHardwareConnected(&Ret);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::IsHardwareConnected: %d"), static_cast<int>(Error));
	return Ret;
}

bool FFoveHMD::IsHardwareReady() const
{
	bool Ret = false;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->IsHardwareReady(&Ret);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::IsHardwareReady: %d"), static_cast<int>(Error));
	return Ret;
}

bool FFoveHMD::IsEyeTrackingCalibrating() const
{
	bool Ret = false;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->IsEyeTrackingCalibrating(&Ret);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::IsEyeTrackingCalibrating: %d"), static_cast<int>(Error));
	return Ret;
}

bool FFoveHMD::EnsureEyeTrackingCalibration()
{
	const Fove::EFVR_ErrorCode Error = FoveHeadset->EnsureEyeTrackingCalibration();
	if (Error != Fove::EFVR_ErrorCode::None)
	{
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::EnsureEyeTrackingCalibration failed: %d"), static_cast<int>(Error));
		return false;
	}
	return true;
}

bool FFoveHMD::GetGazeConvergence(const bool bRelativeToHMD, FVector* const outRayOrigin, FVector* const outRayDirection, float* const outDistance, float* const outAccuracy) const
{
	// Get latest pose. We always use the latest instead of the cached pose for maximum accuracy since the gaze data is the latest.
	FQuat HMDOrientation;
	if (bRelativeToHMD)
	{
		Fove::SFVR_Pose Pose;
		const Fove::EFVR_ErrorCode Error = FoveHeadset->GetHMDPose(&Pose);
		if (Error != Fove::EFVR_ErrorCode::None)
		{
			UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::GetHMDPose failed: %d"), static_cast<int>(Error));
			return false;
		}

		HMDOrientation = ToUnreal(Pose.orientation);
	}

	// Get gaze convergence
	Fove::SFVR_GazeConvergenceData convergence;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->GetGazeConvergence(&convergence);
	if (Error != Fove::EFVR_ErrorCode::None)
	{
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::GetGazeConvergence failed: %d"), static_cast<int>(Error));
		return false;
	}

	if (outRayOrigin)
	{
		*outRayOrigin = ToUnreal(convergence.ray.origin, WorldToMetersScale);
		if (bRelativeToHMD)
			*outRayOrigin = HMDOrientation.RotateVector(*outRayOrigin);
	}

	if (outRayDirection)
	{
		*outRayDirection = ToUnreal(convergence.ray.direction, 1.0f);
		if (bRelativeToHMD)
			*outRayDirection = HMDOrientation.RotateVector(*outRayDirection);
	}

	if (outDistance)
		*outDistance = WorldToMetersScale * convergence.distance;

	if (outAccuracy)
		*outAccuracy = convergence.accuracy;

	return true;
}

bool FFoveHMD::GetGazeVector(const bool bRelativeToHMD, FVector* const outLeft, FVector* const outRight) const
{
	// Get latest pose. We always use the latest instead of the cached pose for maximum accuracy since the gaze data is the latest.
	FQuat HMDOrientation;
	if (bRelativeToHMD)
	{
		Fove::SFVR_Pose Pose;
		const Fove::EFVR_ErrorCode Error = FoveHeadset->GetHMDPose(&Pose);
		if (Error != Fove::EFVR_ErrorCode::None)
		{
			UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::GetHMDPose failed: %d"), static_cast<int>(Error));
			return false;
		}

		HMDOrientation = ToUnreal(Pose.orientation);
	}

	// Get left and/or right gaze
	Fove::SFVR_GazeVector lGaze, rGaze;
	const Fove::EFVR_ErrorCode error = FoveHeadset->GetGazeVectors(outLeft ? &lGaze : nullptr, outRight ? &rGaze : nullptr);
	if (error != Fove::EFVR_ErrorCode::None)
	{
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::GetGazeVectors failed: %d"), static_cast<int>(error));
		return false;
	}

	// Output left gaze
	if (outLeft)
	{
		*outLeft = ToUnreal(lGaze.vector, 1.0f);
		if (!bRelativeToHMD)
			*outLeft = HMDOrientation.RotateVector(*outLeft);
	}

	// Output right gaze
	if (outRight)
	{
		*outRight = ToUnreal(rGaze.vector, 1.0f);
		if (!bRelativeToHMD)
			*outRight = HMDOrientation.RotateVector(*outRight);
	}

	return true;
}

bool FFoveHMD::GetGazeVector2D(FVector2D* const outLeft, FVector2D* const outRight) const
{
	const auto Compute2DGaze = [] (const Fove::SFVR_Matrix44& proj, const Fove::SFVR_Vec3 gaze, FVector2D& out) -> bool
	{
		// Project gaze to get screen coordinates
		const float projX = proj.mat[0][0] * gaze.x + proj.mat[1][0] * gaze.y + proj.mat[2][0] * gaze.z + proj.mat[3][0];
		const float projY = proj.mat[0][1] * gaze.x + proj.mat[1][1] * gaze.y + proj.mat[2][1] * gaze.z + proj.mat[3][1];
		const float projW = proj.mat[0][3] * gaze.x + proj.mat[1][3] * gaze.y + proj.mat[2][3] * gaze.z + proj.mat[3][3];
		out = FVector2D{ projX / projW, projY / projW };
		return true;
	};

	// Get left/right projection
	Fove::SFVR_Matrix44 lProj, rProj;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->GetProjectionMatricesLH(0.01f, 1000.0f, outLeft ? &lProj : nullptr, outRight ? &rProj : nullptr);
	if (Error != Fove::EFVR_ErrorCode::None)
	{
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::GetProjectionMatricesLH failed:  %d"), static_cast<int>(Error));
		return false;
	}

	// Get left and/or right gaze
	Fove::SFVR_GazeVector lGaze, rGaze;
	const Fove::EFVR_ErrorCode error = FoveHeadset->GetGazeVectors(outLeft ? &lGaze : nullptr, outRight ? &rGaze : nullptr);
	if (error != Fove::EFVR_ErrorCode::None)
	{
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::GetGazeVectors failed: %d"), static_cast<int>(error));
		return false;
	}

	FVector2D retLeft, retRight;
	if (outLeft && !Compute2DGaze(lProj, lGaze.vector, retLeft))
		return false;
	if (outRight && !Compute2DGaze(rProj, rGaze.vector, retRight))
		return false;

	if (outLeft)
		*outLeft = retLeft;
	if (outRight)
		*outRight = retRight;

	return true;
}

bool FFoveHMD::ManualDriftCorrection3D(const FVector Location)
{
	const Fove::SFVR_Vec3 vec(Location.Y / WorldToMetersScale, Location.Z / WorldToMetersScale, Location.X / WorldToMetersScale);
	const Fove::EFVR_ErrorCode error = FoveHeadset->ManualDriftCorrection3D(vec);
	return error == Fove::EFVR_ErrorCode::None;
}

bool FFoveHMD::CheckEyesTracked(bool* outLeft, bool* outRight)
{
	Fove::EFVR_Eye eye = Fove::EFVR_Eye::Neither;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->CheckEyesTracked(&eye);
	if (Error != Fove::EFVR_ErrorCode::None)
	{
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::CheckEyesTracked failed: %d"), static_cast<int>(Error));
		return false;
	}

	if (outLeft && (eye == Fove::EFVR_Eye::Both || eye == Fove::EFVR_Eye::Left))
		*outLeft = true;

	if (outRight && (eye == Fove::EFVR_Eye::Both || eye == Fove::EFVR_Eye::Right))
		*outRight = true;

	return true;
}

bool FFoveHMD::CheckEyesClosed(bool* outLeft, bool* outRight)
{
	Fove::EFVR_Eye eye = Fove::EFVR_Eye::Neither;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->CheckEyesClosed(&eye);
	if (Error != Fove::EFVR_ErrorCode::None)
	{
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::CheckEyesClosed failed: %d"), static_cast<int>(Error));
		return false;
	}

	if (outLeft && (eye == Fove::EFVR_Eye::Both || eye == Fove::EFVR_Eye::Left))
		*outLeft = true;

	if (outRight && (eye == Fove::EFVR_Eye::Both || eye == Fove::EFVR_Eye::Right))
		*outRight = true;

	return true;
}

bool FFoveHMD::IsPositionReady() const
{
	bool Ret = false;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->IsPositionReady(&Ret);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::IsPositionReady failed: %d"), static_cast<int>(Error));

	return Ret;
}

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
FName FFoveHMD::GetSystemName() const
{
	static FName name(TEXT("FoveHMD"));
	return name;
}

bool FFoveHMD::EnumerateTrackedDevices(TArray<int, FDefaultAllocator>& OutDevices, const EXRTrackedDeviceType Type)
{
	if (Type == EXRTrackedDeviceType::Any || Type == EXRTrackedDeviceType::HeadMountedDisplay)
	{
		static const int32 DeviceId = IXRTrackingSystem::HMDDeviceId;
		OutDevices.Add(DeviceId);
		return true;
	}
	return false;
}

void FFoveHMD::RefreshPoses()
{
	// TODO
}

bool FFoveHMD::GetCurrentPose(const int32 deviceId, FQuat& OutQuat, FVector& OutVec)
{
	if (deviceId != 0)
		return false;

	PrivOrientationAndPosition(OutQuat, OutVec);
	return true;
}
#endif

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 13 && ENGINE_MINOR_VERSION < 18
FName FFoveHMD::GetDeviceName() const
{
	static FName name(TEXT("FoveHMD"));
	return name;
}
#endif

bool FFoveHMD::IsHMDConnected()
{
	return IsFoveConnected(*FoveHeadset, *FoveCompositor);
}

bool FFoveHMD::IsHMDEnabled() const
{
	return bHmdEnabled;
}

void FFoveHMD::EnableHMD(const bool enable)
{
	// Early out
	if (bHmdEnabled == enable)
		return;

	// The documentation for this function in unreal simply states: "Enables or disables switching to stereo."
	// The meaning of the statement is unclear and could be either:
	//  a) Enables/disables stereo directly
	//  b) Enables/disables the ability to enable stereo (but enabling stereo would be a separate call)
	// We've taken it to mean the latter, so we don't enable stereo when the hmd is enabled
	// However, if you disable the hmd, we no longer have the ability to be in stereo, so we disable that
	if (!enable)
		EnableStereo(false);

	// Update cached state
	// This happens after the call to EnableStereo(false) as that function becomes a no-op when bHmdEnabled is true
	bHmdEnabled = enable;
}

EHMDDeviceType::Type FFoveHMD::GetHMDDeviceType() const
{
	return EHMDDeviceType::DT_ES2GenericStereoMesh;
}

bool FFoveHMD::GetHMDMonitorInfo(MonitorInfo& outInfo)
{
	// Write default values
	outInfo.MonitorName = "";
	outInfo.MonitorId = 0;
	outInfo.DesktopX = outInfo.DesktopY = outInfo.ResolutionX = outInfo.ResolutionY = outInfo.WindowSizeX = outInfo.WindowSizeX = 0;

	// Write resolution
	outInfo.ResolutionX = outInfo.WindowSizeX = FoveCompositorLayer.idealResolutionPerEye.x * 2; // Stereo rendering places the two eyes side by side horizontally
	outInfo.ResolutionY = outInfo.WindowSizeY = FoveCompositorLayer.idealResolutionPerEye.y;

	return true;
}

void FFoveHMD::GetFieldOfView(float& OutHFOVInDegrees, float& OutVFOVInDegrees) const
{
	OutHFOVInDegrees = 0.0f; // TODO
	OutVFOVInDegrees = 0.0f;
}

bool FFoveHMD::IsChromaAbCorrectionEnabled() const
{
	// Note from Unreal after being asked why the engine needs to know this:
	// Generally, we don't!  However, on certain platforms, there are options to turn on and off
	// chromatic aberration correction to trade off performance and quality, which is why we
	// provide the option in the interface.  It's fine to always return true if you're doing it.
	return true;
}

void FFoveHMD::SetInterpupillaryDistance(float NewInterpupillaryDistance)
{
	UE_LOG(LogHMD, Warning, TEXT("FOVE does not support SetInterpupillaryDistance"));
}

float FFoveHMD::GetInterpupillaryDistance() const
{
	// Fetch inter-ocular distance from Fove service
	float Ret = 0.064f; // Sane default in the event of error
	const Fove::EFVR_ErrorCode Error = FoveHeadset->GetIOD(&Ret);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::GetIOD failed: %d"), static_cast<int>(Error));

	return Ret;
}

bool FFoveHMD::DoesSupportPositionalTracking() const
{
	// Todo: Fove supports position tracking in general,
	// but should we query whether the position camera is connected for this?
	return true;
}

bool FFoveHMD::HasValidTrackingPosition()
{
	// Todo: FOVE API has no way to return whether we currently have a valid position, simply that position tracking is running
	bool Ret = false;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->IsPositionReady(&Ret);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::IsPositionReady: %d"), static_cast<int>(Error));
	return Ret;
}

void FFoveHMD::RebaseObjectOrientationAndPosition(FVector& Position, FQuat& Orientation) const
{
	UE_LOG(LogHMD, Warning, TEXT("FOVE does not support RebaseObjectOrientationAndPosition"));
}

bool FFoveHMD::IsHeadTrackingAllowed() const
{
	return GEngine && GEngine->IsStereoscopic3D();
}

void FFoveHMD::ResetOrientationAndPosition(float yaw)
{
	// Note from Unreal about this function:
	// The intention of these functions is to allow the user to reset the calibrated
	// position at any point in the experience.  Generally, this takes the form of
	// saving a base orientation and position, and then using those to modify the
	// pose returned from the SDK as a "poor man's calibration."

	ResetOrientation(yaw);
	ResetPosition();
}

void FFoveHMD::ResetOrientation(float yaw)
{
	// Fixme: what to do with yaw?
	FoveHeadset->TareOrientationSensor();
}

void FFoveHMD::ResetPosition()
{
	FoveHeadset->TarePositionSensors();
}

void FFoveHMD::SetBaseRotation(const FRotator& BaseRot)
{
	BaseOrientation = BaseRot.Quaternion();
}

FRotator FFoveHMD::GetBaseRotation() const
{
	return BaseOrientation.Rotator();
}

void FFoveHMD::SetBaseOrientation(const FQuat& BaseOrient)
{
	BaseOrientation = BaseOrient;
}

FQuat FFoveHMD::GetBaseOrientation() const
{
	return BaseOrientation;
}

void FFoveHMD::OnBeginPlay(FWorldContext& InWorldContext)
{
	EnableStereo(true);
}

void FFoveHMD::OnEndPlay(FWorldContext& InWorldContext)
{
	EnableStereo(false);
}

void FFoveHMD::SetTrackingOrigin(EHMDTrackingOrigin::Type NewOrigin)
{
	// Note from Unreal:
	// This basically allows you to consider the calibrated origin in two locations, depending on the style of game and hardware.
	// EHMDTrackingOrigin::Eye means that the "zero" position is where the player's eyes are.  Floor means that it's on the floor.
	// The difference matters to how people set up their content. Generally, games that require the player to stand use the Floor
	// origin, and the player's pawn is set up so that the Camera Component's parent is located at the bottom of their collision (at the feet).
	// This is nice, because you know that the player's height in game will be the same as their real world height.
	// Alternatively, for games where the player is disembodied, or sitting in a chair, it's more useful to consider the origin of the
	// camera at their eyes, so you can place the parent there.  The player is no longer their true height, but for cockpit games, etc. this doesn't matter.

	switch (NewOrigin)
	{
	case EHMDTrackingOrigin::Eye:
		break;
	default:
		// Fove currently only supports sitting experiences, if a game tries to set this, log a warning
		UE_LOG(LogHMD, Warning, TEXT("FOVE only supports EHMDTrackingOrigin::Eye"));
		break;
	}
}

EHMDTrackingOrigin::Type FFoveHMD::GetTrackingOrigin()
{
	// Currently, FOVE only supports sitting experiences. See comment in SetTrackingOrigin
	return EHMDTrackingOrigin::Eye;
}

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION < 18 // Removed in 4.18

void FFoveHMD::GetPositionalTrackingCameraProperties(FVector&, FQuat&, float&, float&, float&, float&, float&) const
{
	UE_LOG(LogHMD, Warning, TEXT("FOVE does not support GetPositionalTrackingCameraProperties"));
}

void FFoveHMD::GetCurrentOrientationAndPosition(FQuat& CurrentOrientation, FVector& CurrentPosition)
{
	PrivOrientationAndPosition(CurrentOrientation, CurrentPosition);
}

TSharedPtr< class ISceneViewExtension, ESPMode::ThreadSafe > FFoveHMD::GetViewExtension()
{
	TSharedPtr< FFoveHMD, ESPMode::ThreadSafe > ptr(AsShared());
	return StaticCastSharedPtr< ISceneViewExtension >(ptr);
}

void FFoveHMD::ApplyHmdRotation(APlayerController* PC, FRotator& ViewRotation)
{
	ViewRotation.Normalize();

	FQuat hmdOrientation;
	FVector hmdPosition;
	GetCurrentOrientationAndPosition(hmdOrientation, hmdPosition);

	const FRotator DeltaRot = ViewRotation - PC->GetControlRotation();
	ControlRotation = (ControlRotation + DeltaRot).GetNormalized();

	// Pitch from other sources is never good, because there is an absolute up and down that must be respected to avoid motion sickness.
	// Same with roll. Retain yaw by default - mouse/controller based yaw movement still isn't pleasant, but
	// it's necessary for sitting VR experiences.
	ControlRotation.Pitch = 0;
	ControlRotation.Roll = 0;

	ViewRotation = FRotator(ControlRotation.Quaternion() * hmdOrientation);

	AppliedHmdOrientation = FRotator(hmdOrientation);
	AppliedHmdOrientation.Pitch = 0;
	AppliedHmdOrientation.Roll = 0;
}

bool FFoveHMD::UpdatePlayerCamera(FQuat& CurrentOrientation, FVector& CurrentPosition)
{
	FQuat hmdOrientation;
	FVector hmdPosition;
	GetCurrentOrientationAndPosition(hmdOrientation, hmdPosition);

	CurrentOrientation = hmdOrientation;
	CurrentPosition = AppliedHmdOrientation.Quaternion().Inverse().RotateVector(hmdPosition);

	return true;
}

bool FFoveHMD::IsPositionalTrackingEnabled() const
{
	return true;
}
#endif

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION < 16
bool FFoveHMD::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (FParse::Command(&Cmd, TEXT("STEREO")))
	{
		if (FParse::Command(&Cmd, TEXT("ON")))
		{
			if (!IsHMDEnabled())
			{
				Ar.Logf(TEXT("HMD is disabled. Use 'hmd enable' to re-enable it"));
			}
			EnableStereo(true);
			return true;
		}
		else if (FParse::Command(&Cmd, TEXT("OFF")))
		{
			EnableStereo(false);
			return true;
		}
	}
	else if (FParse::Command(&Cmd, TEXT("HMD")))
	{
		if (FParse::Command(&Cmd, TEXT("ENABLE")))
		{
			EnableHMD(true);
			return true;
		}
		else if (FParse::Command(&Cmd, TEXT("DISABLE")))
		{
			EnableHMD(false);
			return true;
		}
	}
	else if (FParse::Command(&Cmd, TEXT("UNCAPFPS")))
	{
		GEngine->bSmoothFrameRate = false;
		return true;
	}
	else if (FParse::Command(&Cmd, TEXT("HEADTRACKING")))
	{
		FString val;
		if (FParse::Value(Cmd, TEXT("SOURCE="), val))
		{
			EnablePositionalTracking(false);
			//OSVRInterfaceName = val;
			EnablePositionalTracking(true);
		}
		if (FParse::Command(&Cmd, TEXT("ENABLE")))
		{
			EnablePositionalTracking(true);
			return true;
		}
		else if (FParse::Command(&Cmd, TEXT("DISABLE")))
		{
			EnablePositionalTracking(false);
			return true;
		}
	}

	return false;
}

bool FFoveHMD::EnablePositionalTracking(bool bEnable)
{
	UE_LOG(LogHMD, Warning, TEXT("FOVE does not support EnablePositionalTracking"));
	return true;
}

bool FFoveHMD::IsInLowPersistenceMode() const
{
	return true; // Not supported, game can think of us as always in low persistence mode
}

void FFoveHMD::EnableLowPersistenceMode(bool bEnable)
{
	UE_LOG(LogHMD, Warning, TEXT("FOVE does not support EnableLowPersistenceMode"));
}

#endif

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
FMatrix FFoveHMD::GetStereoProjectionMatrix(const EStereoscopicPass StereoPass) const
{
	return PrivStereoProjectionMatrix(StereoPass);
}
#endif

void FFoveHMD::SetClippingPlanes(float NCP, float FCP)
{
	ZNear = NCP;
	ZFar = FCP;
}

void FFoveHMD::GetEyeRenderParams_RenderThread(const FRenderingCompositePassContext& Context, FVector2D& EyeToSrcUVScaleValue, FVector2D& EyeToSrcUVOffsetValue) const
{
	if (Context.View.StereoPass == eSSP_LEFT_EYE)
	{
		EyeToSrcUVOffsetValue.X = 0.0f;
		EyeToSrcUVOffsetValue.Y = 0.0f;
	}
	else
	{
		EyeToSrcUVOffsetValue.X = 0.5f;
		EyeToSrcUVOffsetValue.Y = 0.0f;
	}

	EyeToSrcUVScaleValue = FVector2D(0.5f, 1.0f);
}

bool FFoveHMD::IsStereoEnabled() const
{
	check(!bStereoEnabled || bHmdEnabled); // bHmdEnabled must be true for bStereoEnabled to be true
	return bStereoEnabled;
}

bool FFoveHMD::EnableStereo(const bool enable)
{
	// Early out
	if (enable == bStereoEnabled)
		return enable;

	// Don't allow enablement of stereo on while the headset is disabled (see comment in EnableHMD)
	if (!bHmdEnabled)
	{
		check(!bStereoEnabled);
		return false;
	}

	// Edit scene viewport
	if (FSceneViewport* const sceneVP = FoveFindSceneViewport())
	{
		const TSharedPtr<SWindow> window = sceneVP->FindWindow();

		if (enable)
		{
			// If we're enabling stereo rendering, set resolution to headset resolution
			MonitorInfo info;
			if (GetHMDMonitorInfo(info))
			{
				sceneVP->SetViewportSize(info.ResolutionX, info.ResolutionY);
			}
		}
		else if (window.IsValid())
		{
			// If we're disabling stereo rendering, set screen resolution to window size
			FVector2D size = window->GetSizeInScreen();
			sceneVP->SetViewportSize(size.X, size.Y);
			//UE_LOG(LogHMD, Warning, TEXT(text.c_str()));
		}

		// Viewport driven by window only when not in stereo mode
		if (window.IsValid())
			window->SetViewportSizeDrivenByWindow(!enable);
	}

	// Uncap fps to ensure we render at the framerate that FOVE needs
	GEngine->bForceDisableFrameRateSmoothing = enable;

	// Cache state of stereo enablement
	bStereoEnabled = enable;

	return bStereoEnabled;
}

void FFoveHMD::AdjustViewRect(EStereoscopicPass StereoPass, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const
{
	SizeX = SizeX / 2;
	if (StereoPass == eSSP_RIGHT_EYE)
	{
		X += SizeX;
	}
}

void FFoveHMD::GetOrthoProjection(int32 RTWidth, int32 RTHeight, float OrthoDistance, FMatrix OrthoProjection[2]) const
{
	const float HudOffset = 50.0f;
	OrthoProjection[0] = FTranslationMatrix(FVector(HudOffset, 0.0f, 0.0f));
	OrthoProjection[1] = FTranslationMatrix(FVector(-HudOffset + RTWidth * 0.5f, 0.0f, 0.0f));
}

void FFoveHMD::InitCanvasFromView(FSceneView* InView, UCanvas* Canvas)
{
	// Couldn't find any other HMD plugins that do anything here
	// Leaving blank for now
}

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION < 18 // Removed in 4.18

void FFoveHMD::CalculateStereoViewOffset(const EStereoscopicPass StereoPassType, const FRotator& ViewRotation, const float WorldToMeters, FVector& ViewLocation)
{
	if (StereoPassType == eSSP_LEFT_EYE || StereoPassType == eSSP_RIGHT_EYE)
	{
		const float EyeOffset = GetInterpupillaryDistance() * WorldToMeters / (StereoPassType == eSSP_LEFT_EYE ? -2.0f : 2.0f);
		ViewLocation += ViewRotation.Quaternion().RotateVector(FVector(0, EyeOffset, 0));
	}
}

FMatrix FFoveHMD::GetStereoProjectionMatrix(enum EStereoscopicPass StereoPass, const float FOV_ignored) const
{
	return PrivStereoProjectionMatrix(StereoPass);
}

void FFoveHMD::RenderTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FTexture2DRHIParamRef BackBuffer, FTexture2DRHIParamRef SrcTexture) const
{
	check(IsInRenderingThread());

	// Abort if we have not enabled mirroring
	if (WindowMirrorMode == 0)
	{
		return;
	}

	const uint32 ViewportWidth = BackBuffer->GetSizeX();
	const uint32 ViewportHeight = BackBuffer->GetSizeY();

	// Set & clear the render target
	{
		// Need to clear when rendering only one eye since the borders won't be touched by the DrawRect below
		const bool needsToClear = WindowMirrorMode == 1;

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 14
		const ERenderTargetLoadAction loadAction = needsToClear ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ENoAction;
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 16
		FRHIRenderTargetView target(BackBuffer, loadAction);
#else
		FRHIRenderTargetView target(BackBuffer);
		target.LoadAction = loadAction;
#endif
		RHICmdList.SetRenderTargetsAndClear(FRHISetRenderTargetsInfo(1, &target, FRHIDepthRenderTargetView()));
#else
		SetRenderTarget(RHICmdList, BackBuffer, FTextureRHIRef());
#endif

		// Issue clear command on older versions that don't do it with the render target set
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION < 14
		if (needsToClear)
		{
			RHICmdList.Clear(true, FLinearColor::Black, false, 0, false, 0, FIntRect());
		}
#endif

		RHICmdList.SetViewport(0, 0, 0, ViewportWidth, ViewportHeight, 1.0f);
	}

	// Get shaders
	const auto FeatureLevel = GMaxRHIFeatureLevel;
	auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
	TShaderMapRef<FScreenPS> PixelShader(ShaderMap);

	// Set render state
#ifdef FOVE_USE_PIPLINE_STATE_CACHE
	FGraphicsPipelineStateInitializer piplineState;
	RHICmdList.ApplyCachedRenderTargets(piplineState);
	piplineState.BlendState = TStaticBlendState<>::GetRHI();
	piplineState.RasterizerState = TStaticRasterizerState<>::GetRHI();
	piplineState.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

	piplineState.BoundShaderState.VertexDeclarationRHI = RendererModule->GetFilterVertexDeclaration().VertexDeclarationRHI;
	piplineState.BoundShaderState.VertexShaderRHI = GETSAFERHISHADER_VERTEX(*VertexShader);
	piplineState.BoundShaderState.PixelShaderRHI = GETSAFERHISHADER_PIXEL(*PixelShader);
	piplineState.PrimitiveType = PT_TriangleList;

	SetGraphicsPipelineState(RHICmdList, piplineState);
#else
	RHICmdList.SetBlendState(TStaticBlendState<>::GetRHI());
	RHICmdList.SetRasterizerState(TStaticRasterizerState<>::GetRHI());
	RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());

	static FGlobalBoundShaderState BoundShaderState;
	SetGlobalBoundShaderState(RHICmdList, FeatureLevel, BoundShaderState, RendererModule->GetFilterVertexDeclaration().VertexDeclarationRHI, *VertexShader, *PixelShader);
#endif

	// Set shader properties
	PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Bilinear>::GetRHI(), SrcTexture);

	// Draw a rectangle with the content of one or both of the eye images, depending on the mirror mode
	RendererModule->DrawRectangle(
		RHICmdList,
		WindowMirrorMode == 1 ? ViewportWidth / 4 : 0,             // X
		0,                                                         // Y
		WindowMirrorMode == 1 ? ViewportWidth / 2 : ViewportWidth, // SizeX
		ViewportHeight,                                            // SizeY
		WindowMirrorMode == 1 ? 0.1f : 0.0f,                       // U
		WindowMirrorMode == 1 ? 0.2f : 0.0f,                       // V
		WindowMirrorMode == 1 ? 0.3f : 1.0f,                       // SizeU
		WindowMirrorMode == 1 ? 0.6f : 1.0f,                       // SizeV
		FIntPoint(ViewportWidth, ViewportHeight),
		FIntPoint(1, 1),
		*VertexShader,
		EDRF_Default);
}

void FFoveHMD::CalculateRenderTargetSize(const FViewport& Viewport, uint32& InOutSizeX, uint32& InOutSizeY)
{
	check(IsInGameThread());

	//	if (Flags.bScreenPercentageEnabled)
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.ScreenPercentage"));
		float value = CVar->GetValueOnGameThread();
		if (value > 0.0f)
		{
			InOutSizeX = FMath::CeilToInt(InOutSizeX * value / 100.f);
			InOutSizeY = FMath::CeilToInt(InOutSizeY * value / 100.f);
		}
	}
}

bool FFoveHMD::NeedReAllocateViewportRenderTarget(const FViewport& Viewport)
{
	check(IsInGameThread());

	if (IsStereoEnabled())
	{
		const uint32 InSizeX = Viewport.GetSizeXY().X;
		const uint32 InSizeY = Viewport.GetSizeXY().Y;
		FIntPoint RenderTargetSize;
		RenderTargetSize.X = Viewport.GetRenderTargetTexture()->GetSizeX();
		RenderTargetSize.Y = Viewport.GetRenderTargetTexture()->GetSizeY();

		uint32 NewSizeX = InSizeX, NewSizeY = InSizeY;
		CalculateRenderTargetSize(Viewport, NewSizeX, NewSizeY);
		if (NewSizeX != RenderTargetSize.X || NewSizeY != RenderTargetSize.Y)
		{
			return true;
		}
	}
	return false;
}

bool FFoveHMD::ShouldUseSeparateRenderTarget() const
{
	check(IsInGameThread());
	return IsStereoEnabled();
}

void FFoveHMD::UpdateViewport(bool bUseSeparateRenderTarget, const FViewport& InViewport, SViewport* ViewportWidget)
{
	check(IsInGameThread());

	const FViewportRHIRef& viewportRef = InViewport.GetViewportRHI();
	if (viewportRef)
	{
		if (Bridge && IsStereoEnabled())
		{
			viewportRef->SetCustomPresent(Bridge);
			Bridge->UpdateViewport(InViewport);
		}
		else
		{
			viewportRef->SetCustomPresent(nullptr);
		}
	}
}

#endif

void FFoveHMD::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	InViewFamily.EngineShowFlags.MotionBlur = 0;
	InViewFamily.EngineShowFlags.HMDDistortion = false;
	InViewFamily.EngineShowFlags.StereoRendering = IsStereoEnabled();
}

void FFoveHMD::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	PrivOrientationAndPosition(InView.BaseHmdOrientation, InView.BaseHmdLocation);
	WorldToMetersScale = InView.WorldToMetersScale;
	InViewFamily.bUseSeparateRenderTarget = true;
}

void FFoveHMD::PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView)
{
	check(IsInRenderingThread());

	// Update the view rotation with the latest value, sampled just beforehand in PreRenderViewFamily_RenderThread
	if (Bridge)
	{
		const FQuat DeltaOrient = InView.BaseHmdOrientation.Inverse() * Bridge->GetRenderPose().GetRotation();
		InView.ViewRotation = FRotator(InView.ViewRotation.Quaternion() * DeltaOrient);
		InView.UpdateViewMatrix();
	}
}

void FFoveHMD::PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& ViewFamily)
{
	check(IsInRenderingThread());

	if (Bridge)
	{
		// Blocks until the next time we need to render, as determined by the compositor, and fetches a new pose to use during rendering
		// This allows the compositor to cap rendering at exactly the frame rate needed, so we don't draw more frames than the compositor can use
		// Vsync and any other frame rate limiting options within Unreal should be disabled when using with FOVE to ensure this works well
		// This also lets us update the pose just before rendering, so time warp only needs to correct by a small amount
		Fove::SFVR_Pose FovePose;
		const Fove::EFVR_ErrorCode Error = GetCompositor().WaitForRenderPose(&FovePose);
		if (Error != Fove::EFVR_ErrorCode::None)
		{
			UE_LOG(LogHMD, Warning, TEXT("IFVRCompositor::WaitForRenderPose failed: %d"), static_cast<int>(Error));
		}
		else
		{
			// We will be moving the view location just before rendering, so camera-attached objects need a late update to stay locked to the view
			// The API was removed for this so apparently it no longer needs up happen in 4.18+?
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION < 18
			const FTransform LastPose = Bridge->GetRenderPose();
			Bridge->SetRenderPose(FovePose, WorldToMetersScale);
			const FTransform NewPose = Bridge->GetRenderPose();

			ApplyLateUpdate(ViewFamily.Scene, LastPose, NewPose);
#endif
		}
	}
}

void FFoveHMD::PrivOrientationAndPosition(FQuat& OutOrientation, FVector& OutPosition)
{
	checkf(IsInGameThread(), TEXT("PrivOrientationAndPosition called from not game thread"));

	FTransform transform;
	if (Bridge)
	{
		transform = Bridge->GetRenderPose();
	}
	else
	{
		Fove::SFVR_Pose Pose;
		const Fove::EFVR_ErrorCode Error = FoveHeadset->GetHMDPose(&Pose);
		if (Error != Fove::EFVR_ErrorCode::None)
			UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::GetHMDPose failed: %d"), static_cast<int>(Error));

		transform = ToUnreal(Pose, WorldToMetersScale);
	}

	OutOrientation = transform.GetRotation();
	OutPosition = transform.GetLocation();
}

FMatrix FFoveHMD::PrivStereoProjectionMatrix(const EStereoscopicPass StereoPass) const
{
	check(IsStereoEnabled());

	// Query Fove SDK for projection matrix for this eye
	Fove::SFVR_Matrix44 FoveMat;
	const Fove::EFVR_ErrorCode Error = FoveHeadset->GetProjectionMatricesLH(ZNear, ZFar, StereoPass == eSSP_LEFT_EYE ? &FoveMat : nullptr, StereoPass != eSSP_LEFT_EYE ? &FoveMat : nullptr);
	if (Error != Fove::EFVR_ErrorCode::None)
		UE_LOG(LogHMD, Warning, TEXT("IFVRHeadset::IsPositionReady: %d"), static_cast<int>(Error));

	// Convert to Unreal matrix and correct near/far clip (which use reversed-Z in Unreal)
	FMatrix Ret = ToUnreal(FoveMat);
	Ret.M[3][3] = 0.0f;
	Ret.M[2][3] = 1.0f;
	Ret.M[2][2] = ZNear == ZFar ? 0.0f : ZNear / (ZNear - ZFar);
	Ret.M[3][2] = ZNear == ZFar ? ZNear : -ZFar * ZNear / (ZNear - ZFar);

	return Ret;
}

#ifdef _MSC_VER
#pragma endregion
#endif
