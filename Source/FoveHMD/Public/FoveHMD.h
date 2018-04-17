#pragma once

// Unreal headeers
#include "Engine.h"
#include "HeadMountedDisplay.h"
#include "SceneViewExtension.h"
#include "Templates/RefCounting.h"
#include <Runtime/Launch/Resources/Version.h>

// Fove headers
#include "FoveTypes.h"

// Make sure that we have the macros needed to specialize the build for different versions of Unreal Engine
#if !defined(ENGINE_MAJOR_VERSION) || !defined(ENGINE_MINOR_VERSION)
static_assert(false, "Unable to find Unreal version macros");
#endif

// Check the allowed version
// If this check fails, this version of unreal is not supported, but patches are welcome!
#if ENGINE_MAJOR_VERSION != 4 || ENGINE_MINOR_VERSION >= 18 || ENGINE_MINOR_VERSION < 12
static_assert(false, "This version of the Fove Unreal plugin only supports Unreal 4.12 through 4.17");
#endif

// Determine the base class for FFoveHMD
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
#include "HeadMountedDisplayBase.h"
#define FOVEHMD_BASE_CLASS FHeadMountedDisplayBase
#else
#define FOVEHMD_BASE_CLASS IHeadMountedDisplay
#endif

// Forward declarations
struct ID3D11Texture2D;
class FoveRenderingBridge;
class IRendererModule;
namespace Fove
{
	class IFVRHeadset;
	class IFVRCompositor;
}

class FOVEHMD_API FFoveHMD : public FOVEHMD_BASE_CLASS, public ISceneViewExtension, public TSharedFromThis<FFoveHMD, ESPMode::ThreadSafe>
{
public: // Generic

	// Construction / destruction
	FFoveHMD(TSharedRef<Fove::IFVRHeadset, ESPMode::ThreadSafe> headset, TUniquePtr<Fove::IFVRCompositor> compositor, Fove::SFVR_CompositorLayer layer);
	~FFoveHMD() override;

	// Helper to return the global FFoveHMD object
	// Returns null if there is no HMD device, or if the current HMD device is not an FFoveHMD
	static FFoveHMD* Get();

public: // General FOVE-Specific Functions

	// Getters for the FOVE C++ API objects
	// The full FOVE C++ API can be accessed via these helpers
	// Most of the commonly needed functions within the C++ API are exposed through other helpers in this class
	// Such helpers deal with coordinate conversions and Unreal types, so they are more convenient
	Fove::IFVRHeadset&       GetHeadset()       { return *FoveHeadset; }
	Fove::IFVRHeadset const& GetHeadset() const { return *FoveHeadset; }
	Fove::IFVRCompositor&       GetCompositor()       { return *FoveCompositor; }
	Fove::IFVRCompositor const& GetCompositor() const { return *FoveCompositor; }

	//! Returns whether the FOVE headset is connected
	bool IsHardwareConnected() const;

	// Returns true if all the FOVE hardware has been started correctly
	bool IsHardwareReady() const;

public: // Eye tracking

	// Returns true if eye calibration is currently running
	// This generally means that any other content in the headset is at least partially obscured by the calibrator
	bool IsEyeTrackingCalibrating() const;

	// Starts calibration if the current user has no eye tracking calibration
	// This should be invoked at a point in your game before eye tracking is needed,
	// but while the calibration overlay is not a problem (eg. before a level starts).
	// In the event that calibration starts, you can call IsEyeTrackingCalibrating() to determine when it's finished
	// Returns false if there was an error
	bool EnsureEyeTrackingCalibration();

	// Returns the convergence point of the two eye rays via any of non-null out parameters
	// The ray returned will be one of the two eye rays
	// The distance field is the distance along the ray to the intersection point with the other eye ray
	// The accuracy field is an estimatation of the accuracy of the distance field, which may be zero, for example when one eye is blinking/disabled
	// This functionality is considered alpha. While you should use this function for eye tracking, it's best to do a raycast in the 3D world,
	// and only use the distance field (when available) to disambiguate between multiple hits.
	// The coordinates used here are world coordinates with (0,0,0) at the camera point.
	bool GetGazeConvergence(bool bRelativeToHMD, FVector* outRayOrigin, FVector* outRayDirection, float* outDistance, float* outAccuracy) const;

	// Sets outLeft/outRight to the direction of the eye gaze for that eye, if nonnull
	// Returns false if there's an error (output arguments will not be touched in that case)
	// if bRelativeToHMD is true, the rotiation of the HMD will be taken into account
	bool GetGazeVector(bool bRelativeToHMD, FVector* outLeft, FVector* outRight) const;

	// Sets outLeft/outRight to the direction of the eye gaze for that eye, if nonnull
	// The output coordinates are in 0 to 1 coordinates, where (0, 0) is the bottom left and (1, 1) is the top right of the screen
	// Returns false if there's an error (output arguments will not be touched in that case)
	bool GetGazeVector2D(FVector2D* outLeft, FVector2D* outRight) const;

	// Manual drift correction. This is experiemental, dont use it yet
	bool ManualDriftCorrection3D(FVector Location);

	// Sets outLeft/outRigh to true or false based on which eyes are being tracked, if nonnull
	// Returns false if there's an error (output arguments will not be touched in that case)
	bool CheckEyesTracked(bool* outLeft, bool* outRight);

	// Sets outLeft/outRight to true or false based on which eyes are closed, if nonnull
	// Returns false if there's an error (output arguments will not be touched in that case)
	bool CheckEyesClosed(bool* outLeft, bool* outRight);

public: // FOVE-specific position tracking functions

	// Returns true if position tracking hardware has been enabled and initialized
	bool IsPositionReady() const;

public: // IHeadMountedDisplay / FHeadMountedDisplayBase interface

	// 4.18 and later
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
	FName GetSystemName() const override;
	bool EnumerateTrackedDevices(TArray<int, FDefaultAllocator>&, EXRTrackedDeviceType) override;
	void RefreshPoses() override;
	bool GetCurrentPose(int32, FQuat&, FVector&) override;
	float GetWorldToMetersScale() const override { return WorldToMetersScale; }
#endif

	// 4.13 through 4.17
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 13 && ENGINE_MINOR_VERSION < 18
	FName GetDeviceName() const override;
#endif

	// 4.12 and later
	bool IsHMDConnected() override;
	bool IsHMDEnabled() const override;
	void EnableHMD(bool allow = true) override;
	EHMDDeviceType::Type GetHMDDeviceType() const override;
	bool GetHMDMonitorInfo(MonitorInfo&) override;
	void GetFieldOfView(float& OutHFOVInDegrees, float& OutVFOVInDegrees) const override;
	bool IsChromaAbCorrectionEnabled() const override;
	void SetInterpupillaryDistance(float NewInterpupillaryDistance) override;
	float GetInterpupillaryDistance() const override;
	bool DoesSupportPositionalTracking() const override;
	bool HasValidTrackingPosition() override;
	void RebaseObjectOrientationAndPosition(FVector& Position, FQuat& Orientation) const override;
	bool IsHeadTrackingAllowed() const override;
	void ResetOrientationAndPosition(float yaw = 0.f) override;
	void ResetOrientation(float Yaw = 0.f) override;
	void ResetPosition() override;
	void SetBaseRotation(const FRotator& BaseRot) override;
	FRotator GetBaseRotation() const override;
	void SetBaseOrientation(const FQuat& BaseOrient) override;
	FQuat GetBaseOrientation() const override;
	void OnBeginPlay(FWorldContext& InWorldContext) override;
	void OnEndPlay(FWorldContext& InWorldContext) override;
	void SetTrackingOrigin(EHMDTrackingOrigin::Type NewOrigin) override;
	EHMDTrackingOrigin::Type GetTrackingOrigin() override;
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION < 18 // Removed in 4.18
	void GetPositionalTrackingCameraProperties(FVector& OutOrigin, FQuat& OutOrientation, float& OutHFOV, float& OutVFOV, float& OutCameraDistance, float& OutNearPlane, float& OutFarPlane) const override;
	void GetCurrentOrientationAndPosition(FQuat& CurrentOrientation, FVector& CurrentPosition) override;
	TSharedPtr<ISceneViewExtension, ESPMode::ThreadSafe> GetViewExtension() override;
	void ApplyHmdRotation(APlayerController* PC, FRotator& ViewRotation) override;
	bool UpdatePlayerCamera(FQuat& CurrentOrientation, FVector& CurrentPosition) override;
	bool IsPositionalTrackingEnabled() const override;
#endif
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION < 16 // Removed in 4.16
	bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override;
	bool EnablePositionalTracking(bool enable) override;
	bool IsInLowPersistenceMode() const override;
	void EnableLowPersistenceMode(bool Enable = true) override;
#endif

public: // IStereoRendering interface

#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION >= 18
	FMatrix GetStereoProjectionMatrix(EStereoscopicPass) const override;
#endif

	// 4.12 and later
	void SetClippingPlanes(float NCP, float FCP) override;
	void GetEyeRenderParams_RenderThread(const FRenderingCompositePassContext& Context, FVector2D& EyeToSrcUVScaleValue, FVector2D& EyeToSrcUVOffsetValue) const override;
	bool IsStereoEnabled() const override;
	bool EnableStereo(bool stereo = true) override;
	void AdjustViewRect(EStereoscopicPass StereoPass, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const override;
	void GetOrthoProjection(int32 RTWidth, int32 RTHeight, float OrthoDistance, FMatrix OrthoProjection[2]) const override;
	void InitCanvasFromView(FSceneView* InView, UCanvas* Canvas) override;
#if ENGINE_MAJOR_VERSION >= 4 && ENGINE_MINOR_VERSION < 18 // Removed in 4.18
	void CalculateStereoViewOffset(EStereoscopicPass StereoPassType, const FRotator& ViewRotation, const float MetersToWorld, FVector& ViewLocation) override;
	FMatrix GetStereoProjectionMatrix(EStereoscopicPass StereoPassType, const float FOV) const override;
	void RenderTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FTexture2DRHIParamRef BackBuffer, FTexture2DRHIParamRef SrcTexture) const override;
	void CalculateRenderTargetSize(const FViewport& Viewport, uint32& InOutSizeX, uint32& InOutSizeY) override;
	bool NeedReAllocateViewportRenderTarget(const FViewport& Viewport) override;
	bool ShouldUseSeparateRenderTarget() const override;
	void UpdateViewport(bool bUseSeparateRenderTarget, const FViewport& Viewport, SViewport*) override;
#endif

public: // ISceneViewExtension interface

	void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	void PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView) override;
	void PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily) override;

private: // Implementation details

	void PrivOrientationAndPosition(FQuat& OutOrientation, FVector& OutPosition);
	FMatrix PrivStereoProjectionMatrix(EStereoscopicPass) const;

	// Number of "world" units in one meter
	float WorldToMetersScale = 1;

	// Distance to the clip planes. This can be set by the game
	float ZNear = 1.0f;
	float ZFar = 1.0f; // If this is equal to ZNear then there is no far clip
	
	FQuat    BaseOrientation = FQuat::Identity;
	FRotator AppliedHmdOrientation = FRotator(0, 0, 0);
	FRotator ControlRotation = FRotator(0, 0, 0);

	const TSharedRef<Fove::IFVRHeadset, ESPMode::ThreadSafe> FoveHeadset;
	const TSharedRef<Fove::IFVRCompositor, ESPMode::ThreadSafe> FoveCompositor;
	Fove::SFVR_CompositorLayer FoveCompositorLayer;
	IRendererModule* RendererModule = nullptr;

	bool bHmdEnabled = true;
	bool bStereoEnabled = false;

	int32 WindowMirrorMode = 2;  // how to mirror the display contents to the desktop window: 0 - no mirroring, 1 - single eye, 2 - stereo pair

	// The rendering bridge used to submit to the FOVE compositor
	// This is a reference as a hack around sporatic build fails on 4.17+MSVC due to ~FoveRenderingBridge not being defined yet.
	// Even though a forward declaration should be perfectly fine since ~TRefCountPtr<FoveRenderingBridge> is not instanciated until after FoveRenderingBridge is declared...
	TRefCountPtr<FoveRenderingBridge>& Bridge;
};

/*
TODO:
Note from Unreal:
The last comment on this file has to do with eye tracking.  While there's nothing technically wrong with your implementation, there are a few Unreal-isms that you might be able to take advantage of in order to get a more naturally integrated plugin.

Right now, you expose the eye tracking parameters to the player through some functions, which return locations and rotations.  That's useful, but for many users, especially ones that use Blueprint more extensively, it's often more natural and useful for them to think in terms of component based design.  In Unreal, you can have something called SceneComponent, which basically means something that you can add to your Actor, which has a transformation associated with it.  Most things that you can see in the game are components...meshes, sprites, etc.  You can composite them together in Actors in order to create more complex Actors, and they follow a parenting hierarchy.  Because of that, it makes it very easy to deal with them in whichever space you want...component, actor, or world space.

For your eye tracking interface, I think it's fine to leave accessors to get the transforms directly, but you might also consider making a new class based off of SceneComponent, whose job it is to simply modify it's orientation and position to match that of the user's eyes.  That will give users something physical in the world to represent the eye position, and all their standard functions (GetComponentPosition and Orientation, GetComponentWorldOrientation, etc) will also still work.  It also lets them attach things directly to the eye, if they wanted to do a gaze cursor!  All they'd have to do is attach to the eye tracking component, and the rest would be automatically updated.

In the typical set up, I'd imagine that the player would have their character actor, which would in turn have its normal camera, which gets updated by the location and orientation of the HMD.  That part works now!  To do eye tracking, all they would have to do was attach two of your new FoveEyeTrackingComponents, one for each eye, to the camera, and then everything is done!

In order to do this, I suggest looking at MotionControllerComponent, which updates its relative position and orientation constantly based on the position and orientation of the motion controller.  The same could be used for the eye tracking component.  The relative position of the component would be updated to be half of the IPD offset to the left, and the orientation would be the face-space orientation of the eye.  That's it, and it would be a very natural extension of the Unreal component system.
*/
