#pragma once

/*
* Function library for accessing data from the Fove SDK via blueprints
* All functions in here simply forward to FoveHMD
*/

#include "IFoveHMDPlugin.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FoveVRFunctionLibrary.generated.h"

UCLASS()
class UFoveVRFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

public:

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool IsHardwareConnected();

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool IsHardwareReady();

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool IsEyeTrackingCalibrating();

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool EnsureEyeTrackingCalibration();

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool GetGazeConvergence(bool bRelativeToHMD, FVector& outRayOrigin, FVector& outRayDirection, float& outDistance, float& outAccuracy);

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool GetGazeVector(bool bRelativeToHMD, FVector& outLeft, FVector& outRight);

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool GetGazeVector2D(FVector2D& outLeft, FVector2D& outRight);

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool ManualDriftCorrection3D(FVector Location);

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool CheckEyesTracked(bool& outLeft, bool& outRight);

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool CheckEyesClosed(bool& outLeft, bool& outRight);

	UFUNCTION(BlueprintCallable, Category = "FoveVR")
		static bool IsPositionReady();
};
