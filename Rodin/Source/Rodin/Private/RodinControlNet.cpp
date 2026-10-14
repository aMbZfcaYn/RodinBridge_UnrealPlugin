#include "RodinControlNet.h"

#include "AssetExportTask.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace RodinControlNet
{
namespace
{
    struct FMeshInstance
    {
        UStaticMesh* Mesh;
        FTransform Transform;
    };

    bool GatherInstances(AActor* Actor, TArray<FMeshInstance>& OutInstances, FString& OutError)
    {
        if (!IsValid(Actor))
        {
            OutError = TEXT("Select one Actor with static mesh geometry.");
            return false;
        }
        TArray<AActor*> Pending{Actor};
        TSet<AActor*> Visited;
        while (Pending.Num() > 0)
        {
            AActor* Current = Pending.Pop();
            if (!IsValid(Current) || Visited.Contains(Current)) continue;
            Visited.Add(Current);

            TArray<USkeletalMeshComponent*> SkeletalComponents;
            Current->GetComponents(SkeletalComponents);
            for (USkeletalMeshComponent* Component : SkeletalComponents)
            {
                if (IsValid(Component) && Component->GetSkeletalMeshAsset())
                {
                    OutError = TEXT("Skeletal meshes are not supported as scene conditions. Choose an FBX file instead.");
                    return false;
                }
            }

            TArray<UStaticMeshComponent*> Components;
            Current->GetComponents(Components);
            for (UStaticMeshComponent* Component : Components)
            {
                if (!IsValid(Component) || !Component->GetStaticMesh()) continue;
                if (UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
                {
                    for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
                    {
                        FTransform Transform;
                        if (!Instanced->GetInstanceTransform(Index, Transform, true))
                        {
                            OutError = TEXT("Could not read an instanced mesh transform.");
                            return false;
                        }
                        OutInstances.Add({Component->GetStaticMesh(), Transform});
                    }
                }
                else
                {
                    OutInstances.Add({Component->GetStaticMesh(), Component->GetComponentTransform()});
                }
            }
            TArray<AActor*> Children;
            Current->GetAttachedActors(Children);
            Pending.Append(Children);
        }
        if (OutInstances.IsEmpty())
        {
            OutError = TEXT("The selected Actor hierarchy contains no static mesh geometry.");
            return false;
        }
        return true;
    }

    bool BoundsOfInstances(const TArray<FMeshInstance>& Instances, FBox& OutBounds, FString& OutError)
    {
        OutBounds = FBox(ForceInit);
        for (const FMeshInstance& Instance : Instances)
        {
            const FBox Local = Instance.Mesh->GetBoundingBox();
            if (!Local.IsValid || Instance.Transform.ContainsNaN())
            {
                OutError = TEXT("The selected geometry has invalid bounds or transforms.");
                return false;
            }
            const FBox World = TransformLocalBounds(Local.Min, Local.Max, Instance.Transform);
            if (!World.IsValid || World.Min.ContainsNaN() || World.Max.ContainsNaN())
            {
                OutError = TEXT("The selected geometry has invalid world bounds.");
                return false;
            }
            OutBounds += World;
        }
        const FVector Size = OutBounds.GetSize();
        if (!OutBounds.IsValid || Size.ContainsNaN() || (Size.GetMin() <= KINDA_SMALL_NUMBER || Size.GetMax() > MAX_flt / 4.0))
        {
            OutError = TEXT("The selected geometry must have a non-zero size on all three axes.");
            return false;
        }
        return true;
    }

    AActor* GetSelectedActor(FString& OutError)
    {
        USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;
        if (!Selection || Selection->Num() != 1)
        {
            OutError = TEXT("Select exactly one Actor. Attached children are included automatically.");
            return nullptr;
        }
        AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(0));
        if (!IsValid(Actor)) OutError = TEXT("The selection is not a valid level Actor.");
        return Actor;
    }

    enum class ESourceChoice { Cancel, Scene, File };

    ESourceChoice ChooseSource(AActor* Actor)
    {
        if (IsRunningCommandlet() || FApp::IsUnattended() || !FSlateApplication::IsInitialized())
            return ESourceChoice::Cancel;

        ESourceChoice Choice = ESourceChoice::Cancel;
        FBox Bounds(ForceInit);
        FString Error;
        const bool bHasScene = GetActorBounds(Actor, Bounds, Error);
        FString SceneDescription = bHasScene
            ? FString::Printf(TEXT("Scene snapshot: %s\nWorld bounds (cm): %.2f x %.2f x %.2f\nIncludes static mesh components and attached children; instance transforms are baked."),
                *Actor->GetActorLabel(), Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z)
            : TEXT("Scene snapshot unavailable. Select exactly one Actor with supported static mesh geometry.");

        const TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(NSLOCTEXT("Rodin", "ConditionTitle", "Choose ControlNet condition"))
            .SizingRule(ESizingRule::Autosized).SupportsMinimize(false).SupportsMaximize(false);
        const TWeakPtr<SWindow> WeakWindow = Window;
        auto Choose = [&Choice, WeakWindow](ESourceChoice Source)
        {
            Choice = Source;
            if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow();
            return FReply::Handled();
        };
        Window->SetContent(
            SNew(SBox).WidthOverride(540).Padding(16)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                [ SNew(STextBlock).AutoWrapText(true).Text(NSLOCTEXT("Rodin", "ConditionExplanation",
                    "No scene mesh is uploaded automatically. Choose a scene geometry snapshot or an external FBX file for this submission.")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
                [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(SceneDescription)) ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [ SNew(SButton).IsEnabled(bHasScene)
                        .Text(NSLOCTEXT("Rodin", "SceneCondition", "Use scene snapshot"))
                        .OnClicked_Lambda([Choose]() { return Choose(ESourceChoice::Scene); }) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [ SNew(SButton).Text(NSLOCTEXT("Rodin", "FileCondition", "Choose FBX file"))
                        .OnClicked_Lambda([Choose]() { return Choose(ESourceChoice::File); }) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(NSLOCTEXT("Rodin", "CancelCondition", "Cancel"))
                        .OnClicked_Lambda([Choose]() { return Choose(ESourceChoice::Cancel); }) ]
                ]
            ]);
        FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().FindBestParentWindowForDialogs(nullptr));
        return Choice;
    }

    bool ExportAndRead(FMeshDescription& Description, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
    {
        UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
        Mesh->GetStaticMaterials().Add(FStaticMaterial());
        UStaticMesh::FBuildMeshDescriptionsParams Params;
        Params.bMarkPackageDirty = false;
        Params.bUseHashAsGuid = true;
        Params.bFastBuild = false;
        const TArray<const FMeshDescription*> Descriptions{&Description};
        if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params))
        {
            OutError = TEXT("Could not build the ControlNet geometry snapshot.");
            return false;
        }
        const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Rodin/Conditions"));
        if (!IFileManager::Get().MakeDirectory(*Directory, true))
        {
            OutError = TEXT("Could not create the ControlNet temporary directory.");
            return false;
        }
        const FString Filename = Directory / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".fbx"));
        UAssetExportTask* Task = NewObject<UAssetExportTask>();
        UFbxExportOption* Options = NewObject<UFbxExportOption>(Task);
        Options->bASCII = false;
        Options->bForceFrontXAxis = false;
        Options->LevelOfDetail = false;
        Options->Collision = false;
        Task->Object = Mesh;
        Task->Filename = Filename;
        Task->bAutomated = true;
        Task->bPrompt = false;
        Task->bReplaceIdentical = false;
        Task->Options = Options;
        const bool bExported = UExporter::RunAssetExportTask(Task);
        const bool bRead = bExported && ReadFbxCondition(Filename, OutJson, OutError);
        // Bytes have been copied into JSON; no future request relies on this file.
        IFileManager::Get().Delete(*Filename, false, true);
        if (!bExported) OutError = TEXT("Could not export the ControlNet condition as FBX.");
        return bRead;
    }
}

EMode ParseMode(const FString& Mode)
{
    const FString Value = Mode.TrimStartAndEnd();
    if (Value.Equals(TEXT("Image"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EMode::Image;
    if (Value.Equals(TEXT("bbox"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("BoundingBox"), ESearchCase::IgnoreCase)) return EMode::BoundingBox;
    if (Value.Equals(TEXT("voxel"), ESearchCase::IgnoreCase)) return EMode::Voxel;
    if (Value.Equals(TEXT("pointCloud"), ESearchCase::IgnoreCase)) return EMode::PointCloud;
    return EMode::Invalid;
}

FBox TransformLocalBounds(const FVector& LocalMin, const FVector& LocalMax, const FTransform& Transform)
{
    return FBox(LocalMin, LocalMax).TransformBy(Transform);
}

bool GetActorBounds(AActor* Actor, FBox& OutBounds, FString& OutError)
{
    OutError.Reset();
    OutBounds = FBox(ForceInit);
    TArray<FMeshInstance> Instances;
    return GatherInstances(Actor, Instances, OutError) && BoundsOfInstances(Instances, OutBounds, OutError);
}

bool BuildBoundingBoxMesh(const FBox& Bounds, FMeshDescription& OutMesh, FString& OutError)
{
    OutError.Reset();
    OutMesh.Empty();
    if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() || Bounds.GetSize().GetMin() <= KINDA_SMALL_NUMBER)
    {
        OutError = TEXT("A bounding-box condition must have a finite, non-zero size.");
        return false;
    }
    FStaticMeshAttributes Attributes(OutMesh);
    Attributes.Register();
    const auto Positions = Attributes.GetVertexPositions();
    auto UVs = Attributes.GetVertexInstanceUVs();
    UVs.SetNumChannels(1);
    const FVector Half = Bounds.GetExtent();
    // Recenter the proxy: the scene's absolute location is not a generation condition.
    const FVector Corners[8] = {
        {-Half.X, -Half.Y, -Half.Z}, {Half.X, -Half.Y, -Half.Z}, {Half.X, Half.Y, -Half.Z}, {-Half.X, Half.Y, -Half.Z},
        {-Half.X, -Half.Y, Half.Z}, {Half.X, -Half.Y, Half.Z}, {Half.X, Half.Y, Half.Z}, {-Half.X, Half.Y, Half.Z}
    };
    FVertexID Vertices[8];
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Vertices[Index] = OutMesh.CreateVertex();
        Positions[Vertices[Index]] = FVector3f(Corners[Index]);
    }
    const FPolygonGroupID Group = OutMesh.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Condition");
    const int32 Faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    const FVector2f FaceUVs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const auto& Face : Faces)
    {
        TArray<FVertexInstanceID> Instances;
        for (int32 Index = 0; Index < 4; ++Index)
        {
            const FVertexInstanceID Instance = OutMesh.CreateVertexInstance(Vertices[Face[Index]]);
            UVs.Set(Instance, 0, FaceUVs[Index]);
            Attributes.GetVertexInstanceColors()[Instance] = FVector4f(1, 1, 1, 1);
            Instances.Add(Instance);
        }
        OutMesh.CreatePolygon(Group, Instances);
    }
    FStaticMeshOperations::ComputeTriangleTangentsAndNormals(OutMesh);
    FStaticMeshOperations::ComputeTangentsAndNormals(OutMesh, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);
    return true;
}

bool BuildSceneMesh(AActor* Actor, FMeshDescription& OutMesh, FBox& OutBounds, FString& OutError)
{
    OutError.Reset();
    OutBounds = FBox(ForceInit);
    OutMesh.Empty();
    TArray<FMeshInstance> Instances;
    if (!GatherInstances(Actor, Instances, OutError) || !BoundsOfInstances(Instances, OutBounds, OutError)) return false;
    FStaticMeshAttributes(OutMesh).Register();
    for (const FMeshInstance& Instance : Instances)
    {
        const FMeshDescription* Source = Instance.Mesh->GetMeshDescription(0);
        if (!Source || Source->Triangles().Num() == 0)
        {
            OutError = FString::Printf(TEXT("'%s' has no editable LOD0 source geometry. Choose an external FBX file."), *Instance.Mesh->GetName());
            OutMesh.Empty();
            return false;
        }
        FStaticMeshOperations::FAppendSettings Settings;
        FTransform CenteredTransform = Instance.Transform;
        CenteredTransform.AddToTranslation(-OutBounds.GetCenter());
        Settings.MeshTransform = CenteredTransform;
        FStaticMeshOperations::AppendMeshDescription(*Source, OutMesh, Settings);
    }
    // A condition carries shape only, not the source Actor's material slots.
    for (const FPolygonGroupID Group : OutMesh.PolygonGroups().GetElementIDs())
        FStaticMeshAttributes(OutMesh).GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Condition");
    FStaticMeshOperations::ComputeTriangleTangentsAndNormals(OutMesh);
    FStaticMeshOperations::ComputeTangentsAndNormals(OutMesh, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);
    return true;
}

bool ReadFbxCondition(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    OutJson.Reset();
    OutError.Reset();
    if (!FPaths::GetExtension(FilePath).Equals(TEXT("fbx"), ESearchCase::IgnoreCase))
    {
        OutError = TEXT("Only FBX files are supported as ControlNet conditions.");
        return false;
    }
    const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
    if (FileSize <= 0 || FileSize > MaxConditionFileBytes)
    {
        OutError = TEXT("The condition FBX must exist, be non-empty, and be no larger than 64 MiB.");
        return false;
    }
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FilePath) || Bytes.IsEmpty() || Bytes.Num() > MaxConditionFileBytes)
    {
        OutError = TEXT("Could not read the condition FBX file.");
        return false;
    }
    static const ANSICHAR BinaryHeader[] = "Kaydara FBX Binary  \0\x1a\0";
    const bool bBinary = Bytes.Num() >= 23 && FMemory::Memcmp(Bytes.GetData(), BinaryHeader, 23) == 0;
    bool bAscii = false;
    if (!bBinary)
    {
        const FUTF8ToTCHAR Header(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), FMath::Min(Bytes.Num(), 1024));
        bAscii = FString(Header.Length(), Header.Get()).Contains(TEXT("FBXHeaderExtension"));
    }
    if (!bBinary && !bAscii)
    {
        OutError = TEXT("The selected file does not contain an FBX header.");
        return false;
    }
    OutJson = MakeShared<FJsonObject>();
    OutJson->SetStringField(TEXT("format"), TEXT("fbx"));
    OutJson->SetNumberField(TEXT("length"), Bytes.Num());
    OutJson->SetStringField(TEXT("md5"), FMD5::HashBytes(Bytes.GetData(), Bytes.Num()));
    OutJson->SetStringField(TEXT("content"), TEXT("data:model/fbx;base64,") + FBase64::Encode(Bytes));
    return true;
}

bool CreateActorCondition(EMode Mode, AActor* Actor, FCondition& OutCondition, FString& OutError)
{
    OutCondition = FCondition{};
    OutError.Reset();
    FBox Bounds(ForceInit);
    FMeshDescription Description;
    if (Mode == EMode::BoundingBox)
    {
        if (!GetActorBounds(Actor, Bounds, OutError) || !BuildBoundingBoxMesh(Bounds, Description, OutError)) return false;
    }
    else if (Mode == EMode::Voxel || Mode == EMode::PointCloud)
    {
        if (!BuildSceneMesh(Actor, Description, Bounds, OutError)) return false;
    }
    else
    {
        OutError = TEXT("This mode does not accept a scene condition.");
        return false;
    }
    if (!ExportAndRead(Description, OutCondition.Json, OutError)) return false;
    OutCondition.HeightInMeters = static_cast<float>(Bounds.GetSize().Z / 100.0);
    return true;
}

bool PrepareForSubmission(const FString& Mode, const FString& FilePath, FCondition& OutCondition, FString& OutError)
{
    OutCondition = FCondition{};
    OutError.Reset();
    const EMode Parsed = ParseMode(Mode);
    if (Parsed == EMode::Image)
    {
        OutCondition.Json = MakeShared<FJsonObject>();
        return true;
    }
    if (Parsed == EMode::Invalid)
    {
        OutError = FString::Printf(TEXT("Unsupported ControlNet mode: %s"), *Mode);
        return false;
    }
    if (Parsed == EMode::BoundingBox)
    {
        AActor* Actor = GetSelectedActor(OutError);
        return Actor && CreateActorCondition(Parsed, Actor, OutCondition, OutError);
    }
    if (!FilePath.IsEmpty()) return ReadFbxCondition(FilePath, OutCondition.Json, OutError);

    FString SelectionError;
    AActor* Actor = GetSelectedActor(SelectionError);
    const ESourceChoice Choice = ChooseSource(Actor);
    if (Choice == ESourceChoice::Scene) return CreateActorCondition(Parsed, Actor, OutCondition, OutError);
    if (Choice == ESourceChoice::File)
    {
        IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
        TArray<FString> Files;
        const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
        if (Desktop && Desktop->OpenFileDialog(Parent, TEXT("Choose ControlNet condition FBX"), TEXT(""), TEXT(""),
            TEXT("FBX model (*.fbx)|*.fbx"), 0, Files) && Files.Num() == 1)
            return ReadFbxCondition(Files[0], OutCondition.Json, OutError);
    }
    OutError = TEXT("ControlNet submission cancelled: choose a scene snapshot or an FBX condition explicitly.");
    return false;
}
}
