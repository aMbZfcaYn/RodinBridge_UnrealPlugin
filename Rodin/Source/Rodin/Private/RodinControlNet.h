#pragma once

#include "CoreMinimal.h"

class AActor;
class FJsonObject;
struct FMeshDescription;

namespace RodinControlNet
{
    enum class EMode : uint8 { Image, BoundingBox, Voxel, PointCloud, Invalid };

    // The limit applies before base64 encoding, not to the WebSocket frame size.
    constexpr int64 MaxConditionFileBytes = 64 * 1024 * 1024;

    struct FCondition
    {
        TSharedPtr<FJsonObject> Json;
        TOptional<float> HeightInMeters;
    };

    EMode ParseMode(const FString& Mode);
    FBox TransformLocalBounds(const FVector& LocalMin, const FVector& LocalMax, const FTransform& Transform);
    bool GetActorBounds(AActor* Actor, FBox& OutBounds, FString& OutError);
    bool BuildBoundingBoxMesh(const FBox& Bounds, FMeshDescription& OutMesh, FString& OutError);
    bool BuildSceneMesh(AActor* Actor, FMeshDescription& OutMesh, FBox& OutBounds, FString& OutError);
    bool ReadFbxCondition(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    bool CreateActorCondition(EMode Mode, AActor* Actor, FCondition& OutCondition, FString& OutError);

    // Image never reads FilePath. BoundingBox uses scene bounds, never the old mesh path.
    // Voxel/PointCloud require an explicit file or an interactive source choice.
    bool PrepareForSubmission(const FString& Mode, const FString& FilePath,
        FCondition& OutCondition, FString& OutError);
}
