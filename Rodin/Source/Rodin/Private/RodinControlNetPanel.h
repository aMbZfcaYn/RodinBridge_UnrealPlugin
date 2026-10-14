#pragma once
#include "CoreMinimal.h"
class UBlueprint;

namespace RodinControlNetPanel
{
    // Compatibility adapter for the shipped UE 5.1 panels. Never saves a package.
    // Fail closed if the graph no longer matches the verified legacy layout.
    bool Apply(UBlueprint* Panel, FString& OutError);
}
