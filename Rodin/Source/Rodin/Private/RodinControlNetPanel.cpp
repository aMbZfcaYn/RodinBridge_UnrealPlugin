#include "RodinControlNetPanel.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace RodinControlNetPanel
{
namespace
{
    UEdGraphNode* SingleSource(UEdGraphNode* Node)
    {
        UEdGraphPin* Execute = Node ? Node->FindPin(TEXT("execute"), EGPD_Input) : nullptr;
        return Execute && Execute->LinkedTo.Num() == 1 ? Execute->LinkedTo[0]->GetOwningNode() : nullptr;
    }

    bool IsScaleDivisionNode(const UEdGraphNode* Node)
    {
        if (!Node) return false;
        const FString ClassName = Node->GetClass()->GetName();
        if (ClassName.Contains(TEXT("PromotableOperator"))) return true;
        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            return Call->GetFunctionName().ToString().Contains(TEXT("Divide"));
        return false;
    }

    bool HasZHeightScaleFix(UBlueprint* Panel)
    {
        for (UEdGraph* Graph : Panel->FunctionGraphs)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node->NodeComment == TEXT("Rodin.ControlNet.UseZHeight")) return true;
            }
        }
        return false;
    }

    bool ApplyZHeightScaleFix(UBlueprint* Panel, FString& OutError)
    {
        UEdGraphPin* SizeZ = nullptr;
        UK2Node_Knot* ScaleKnot = nullptr;
        for (UEdGraph* Graph : Panel->FunctionGraphs)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
                if (!Call || Call->GetFunctionName() != TEXT("BP_actorSize")) continue;
                UEdGraphPin* CandidateY = Call->FindPin(TEXT("sizeY"), EGPD_Output);
                UEdGraphPin* CandidateZ = Call->FindPin(TEXT("sizeZ"), EGPD_Output);
                if (!CandidateY || !CandidateZ) continue;
                for (UEdGraphPin* Linked : CandidateY->LinkedTo)
                {
                    UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Linked->GetOwningNode());
                    if (!Knot) continue;
                    UEdGraphPin* KnotOutput = Knot->FindPin(TEXT("OutputPin"), EGPD_Output);
                    if (!KnotOutput) continue;
                    bool bFeedsScaleDivision = false;
                    for (UEdGraphPin* Downstream : KnotOutput->LinkedTo)
                    {
                        if (IsScaleDivisionNode(Downstream->GetOwningNode()))
                        {
                            bFeedsScaleDivision = true;
                            break;
                        }
                    }
                    if (bFeedsScaleDivision)
                    {
                        SizeZ = CandidateZ;
                        ScaleKnot = Knot;
                        break;
                    }
                }
                if (ScaleKnot) break;
            }
            if (ScaleKnot) break;
        }
        if (!ScaleKnot || !SizeZ)
        {
            OutError = TEXT("Could not find the legacy import scale path that uses BP_actorSize.sizeY. No graph changes were made.");
            return false;
        }
        UEdGraphPin* KnotInput = ScaleKnot->FindPin(TEXT("InputPin"), EGPD_Input);
        if (!KnotInput)
        {
            OutError = TEXT("The legacy import scale node has no input pin. No graph changes were made.");
            return false;
        }
        KnotInput->BreakAllPinLinks();
        SizeZ->MakeLinkTo(KnotInput);
        ScaleKnot->NodeComment = TEXT("Rodin.ControlNet.UseZHeight");
        return true;
    }

}

bool Apply(UBlueprint* Panel, FString& OutError)
{
    OutError.Reset();
    if (!Panel)
    {
        OutError = TEXT("The Rodin panel is unavailable.");
        return false;
    }
    UEdGraphNode* ExportNode = nullptr;
    UK2Node_IfThenElse* AdaptedModeBranch = nullptr;
    TArray<UK2Node_CallFunction*> SubmitNodes;
    const bool bScaleAlreadyAdapted = HasZHeightScaleFix(Panel);
    for (UEdGraph* Graph : Panel->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->NodeComment == TEXT("Rodin.ControlNet.NativePreparation"))
                AdaptedModeBranch = Cast<UK2Node_IfThenElse>(Node);
            UEdGraphPin* Script = Node->FindPin(TEXT("PythonScript"), EGPD_Input);
            if (Script && Script->DefaultValue.Contains(TEXT("exportFirstSelectedStaticMeshAsFBX")))
            {
                if (ExportNode)
                {
                    OutError = TEXT("The panel has multiple legacy mesh exporters; it needs an explicit migration.");
                    return false;
                }
                ExportNode = Node;
            }
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            {
                if (Call->GetFunctionName() == TEXT("ST_SubmitInfo") || Call->GetFunctionName() == TEXT("ST_SubmitInfo_Multi"))
                    SubmitNodes.Add(Call);
            }
        }
    }
    if (!ExportNode || SubmitNodes.Num() != 2)
    {
        OutError = TEXT("The panel is not a supported legacy ControlNet layout. No graph changes were made.");
        return false;
    }
    UEdGraphNode* SelectionBranch = SingleSource(ExportNode);
    UEdGraphNode* SelectionCall = SingleSource(SelectionBranch);
    UK2Node_IfThenElse* ModeBranch = AdaptedModeBranch ? AdaptedModeBranch : Cast<UK2Node_IfThenElse>(SingleSource(SelectionCall));
    UEdGraphPin* Then = ModeBranch ? ModeBranch->FindPin(TEXT("then"), EGPD_Output) : nullptr;
    UEdGraphPin* Else = ModeBranch ? ModeBranch->FindPin(TEXT("else"), EGPD_Output) : nullptr;
    if (!Then || !Else || Then->LinkedTo.Num() != 1 || Else->LinkedTo.Num() != 1)
    {
        OutError = TEXT("Could not verify the legacy scene-export branch. No graph changes were made.");
        return false;
    }
    UEdGraphPin* Condition = ModeBranch->FindPin(TEXT("Condition"), EGPD_Input);
    UEdGraphNode* ModeComparison = Condition && Condition->LinkedTo.Num() == 1 ? Condition->LinkedTo[0]->GetOwningNode() : nullptr;
    UEdGraphPin* ImageValue = ModeComparison ? ModeComparison->FindPin(TEXT("B"), EGPD_Input) : nullptr;
    if (!ImageValue || ImageValue->DefaultValue != TEXT("Image") || !ImageValue->LinkedTo.IsEmpty())
    {
        OutError = TEXT("Could not verify the panel's Image mode check. No graph changes were made.");
        return false;
    }
    UEdGraphPin* Continuation = Then->LinkedTo[0];
    bool bAlreadyAdapted = Else->LinkedTo[0] == Continuation;
    if (!bAlreadyAdapted && Else->LinkedTo[0]->GetOwningNode() != SelectionCall)
    {
        OutError = TEXT("The scene-export branch has changed. No graph changes were made.");
        return false;
    }
    // Preflight both submission sites before editing anything.
    for (UK2Node_CallFunction* Submit : SubmitNodes)
    {
        UEdGraphPin* Next = Submit->FindPin(TEXT("then"), EGPD_Output);
        UEdGraphPin* Success = Submit->FindPin(TEXT("sendSuccess"), EGPD_Output);
        UEdGraphPin* Path = Submit->FindPin(TEXT("modelFilePath"), EGPD_Input);
        if (!Next || Next->LinkedTo.Num() != 1 || !Success || !Path)
        {
            OutError = TEXT("Could not verify the legacy submission pins. No graph changes were made.");
            return false;
        }
        bAlreadyAdapted &= Next->LinkedTo[0]->GetOwningNode()->NodeComment == TEXT("Rodin.ControlNet.SuccessGate") && Path->LinkedTo.IsEmpty() && Path->DefaultValue.IsEmpty();
    }
    if (bAlreadyAdapted && bScaleAlreadyAdapted)
    {
        if (Panel->Status != BS_Error) return true;
        OutError = TEXT("The adapted Rodin panel is in a compilation error state. Reload the asset before retrying.");
        return false;
    }

    UPackage* Package = Panel->GetOutermost();
    const bool bWasDirty = Package->IsDirty();
    // Both Image and ControlNet now go directly to the existing prompt/image checks.
    // Condition preparation is performed once by the native submission function.
    if (!bAlreadyAdapted)
    {
        ModeBranch->NodeComment = TEXT("Rodin.ControlNet.NativePreparation");
        Else->BreakAllPinLinks();
        Else->MakeLinkTo(Continuation);
    }
    if (!bAlreadyAdapted)
    {
    for (UK2Node_CallFunction* Submit : SubmitNodes)
    {
        UEdGraphPin* Path = Submit->FindPin(TEXT("modelFilePath"), EGPD_Input);
        Path->BreakAllPinLinks();
        Path->DefaultValue.Reset();

        UEdGraphPin* Next = Submit->FindPin(TEXT("then"), EGPD_Output);
        if (Next->LinkedTo[0]->GetOwningNode()->NodeComment == TEXT("Rodin.ControlNet.SuccessGate")) continue;
        UEdGraphPin* OldContinuation = Next->LinkedTo[0];
        UEdGraph* Graph = Submit->GetGraph();
        UK2Node_IfThenElse* Gate = NewObject<UK2Node_IfThenElse>(Graph);
        Graph->AddNode(Gate, false, false);
        Gate->CreateNewGuid();
        Gate->NodePosX = Submit->NodePosX + 240;
        Gate->NodePosY = Submit->NodePosY;
        Gate->NodeComment = TEXT("Rodin.ControlNet.SuccessGate");
        Gate->AllocateDefaultPins();
        Next->BreakAllPinLinks();
        Next->MakeLinkTo(Gate->FindPinChecked(TEXT("execute")));
        Submit->FindPinChecked(TEXT("sendSuccess"))->MakeLinkTo(Gate->FindPinChecked(TEXT("Condition")));
        Gate->FindPinChecked(TEXT("then"))->MakeLinkTo(OldContinuation);
        // The false output deliberately has no continuation: no willSend or UI lock.
    }
    }
    if (!bScaleAlreadyAdapted && !ApplyZHeightScaleFix(Panel, OutError)) return false;
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(Panel, EBlueprintCompileOptions::None, &Results);
    Package->SetDirtyFlag(bWasDirty);
    if (Results.NumErrors > 0 || Panel->Status == BS_Error)
    {
        OutError = TEXT("The adapted Rodin panel could not compile. It will not be opened.");
        return false;
    }
    return true;
}
}
