// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "NiagaraNode.h"
#include "NiagaraGraph.h"
#include "EdGraphSchema_Niagara.h"
#include "INiagaraCompiler.h"
#include "GraphEditAction.h"

#define LOCTEXT_NAMESPACE "NiagaraNode"

UNiagaraNode::UNiagaraNode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UNiagaraNode::PostLoad()
{
	Super::PostLoad();

	if (GIsEditor && HasAllFlags(RF_Transactional) == false)
	{
		SetFlags(RF_Transactional);
	}
}

bool UNiagaraNode::ReallocatePins()
{
	Modify();

	// Move the existing pins to a saved array
	TArray<UEdGraphPin*> OldPins(Pins);
	Pins.Reset();

	// Recreate the new pins
	AllocateDefaultPins();

	// Determine if the pins are the same as they were previously...
	bool bAllSame = OldPins.Num() == Pins.Num();

	// Copy the old pin data and remove it.
	for (int32 OldPinIndex = 0; OldPinIndex < OldPins.Num(); ++OldPinIndex)
	{
		UEdGraphPin* OldPin = OldPins[OldPinIndex];

		// When matching pins, use the pin id if either pin id is valid, otherwise match by name.
		auto PinMatchPredicate = [&](UEdGraphPin* Pin) 
		{
			bool bPinsHaveIds = Pin->PersistentGuid.IsValid() || OldPin->PersistentGuid.IsValid();
			return Pin->Direction == OldPin->Direction && ((bPinsHaveIds && Pin->PersistentGuid == OldPin->PersistentGuid) || (bPinsHaveIds == false && Pin->PinName == OldPin->PinName));
		};

		OldPin->Modify();
		if (UEdGraphPin** MatchingNewPin = Pins.FindByPredicate(PinMatchPredicate))
		{
			// If the pin types don't match, CopyPersistentDataFromOldPin could very well overwrite our Matching pin with bad data.
			// Let's cache it for now and reset it after copying the other relevant data off the old pin.
			FString DefaultValue;
			FString AutogeneratedDefaultValue;
			class UObject* DefaultObject = nullptr;
			FText DefaultTextValue;
			bool bTypeMismatch = (*MatchingNewPin)->PinType != OldPin->PinType;

			const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
			check(Schema);

			FNiagaraTypeDefinition OldPinNiagaraType = Schema->PinToTypeDefinition(OldPin);
			FNiagaraTypeDefinition NewPinNiagaraType = Schema->PinToTypeDefinition(*MatchingNewPin);

			bool bRetainOldTypeDueToNumerics = OldPinNiagaraType != FNiagaraTypeDefinition::GetGenericNumericDef() && NewPinNiagaraType == FNiagaraTypeDefinition::GetGenericNumericDef();

			if (bTypeMismatch && !bRetainOldTypeDueToNumerics)
			{
				DefaultValue = (*MatchingNewPin)->DefaultValue;
				AutogeneratedDefaultValue = (*MatchingNewPin)->AutogeneratedDefaultValue;
				DefaultObject = (*MatchingNewPin)->DefaultObject;
				DefaultTextValue = (*MatchingNewPin)->DefaultTextValue;
			}
			
			// This copies the existing default values, pin linkages, advanced pin view, pin splitting, etc.
			(*MatchingNewPin)->MovePersistentDataFromOldPin(*OldPin);

			// The prior call would have clobbered our default values, which causes a crash down the line when we attempt to compile.
			// This resets to the default values prior to copying over the persistent data.
			// @TODO Make this try to preserve as much of the old default values as possible.
			// @TODO Should we push this up to CopyPersistentDataFromOldPin globally?
			if (bTypeMismatch && !bRetainOldTypeDueToNumerics)
			{
				(*MatchingNewPin)->DefaultValue = DefaultValue;
				(*MatchingNewPin)->AutogeneratedDefaultValue = AutogeneratedDefaultValue;
				(*MatchingNewPin)->DefaultObject = DefaultObject;
				(*MatchingNewPin)->DefaultTextValue = DefaultTextValue;
				bAllSame = false;
			}
			else if (bTypeMismatch && bRetainOldTypeDueToNumerics)
			{
				(*MatchingNewPin)->PinType = OldPin->PinType;
			}
			
		}
		else
		{
			bAllSame = false;
		}
		OldPin->MarkPendingKill();
	}

	GetGraph()->NotifyGraphChanged();
	if (!bAllSame)
	{
		CastChecked<UNiagaraGraph>(GetGraph())->NotifyGraphNeedsRecompile();
	}
	return bAllSame;
}

int32 UNiagaraNode::CompileInputPin(INiagaraCompiler* Compiler, UEdGraphPin* Pin)
{
	return Compiler->CompilePin(Pin);
}

bool UNiagaraNode::CompileInputPins(INiagaraCompiler* Compiler, TArray<int32>& OutCompiledInputs)
{
	bool bError = false;
	
	TArray<UEdGraphPin*> InputPins;
	GetInputPins(InputPins);

	for (int32 i = 0; i < InputPins.Num(); ++i)
	{
		UEdGraphPin* Pin = InputPins[i];
		check(Pin->Direction == EGPD_Input);
		int32 Result = CompileInputPin(Compiler, Pin);
		if (Result == INDEX_NONE)
		{
			bError = true;
			Compiler->Error(FText::Format(LOCTEXT("CompileInputPinErrorFormat", "Error compiling Pin"), Pin->PinFriendlyName), this, Pin);
		}

		OutCompiledInputs.Add(Result);
	}
	return bError;
}

void UNiagaraNode::AutowireNewNode(UEdGraphPin* FromPin)
{
	Super::AutowireNewNode(FromPin);

	if (FromPin != nullptr)
	{
		const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
		check(Schema);
		
		//ENiagaraCompoundType FromType = Schema->GetPinDataType(FromPin);

		//Find first of this nodes pins with the right type and direction.
		UEdGraphPin* FirstPinOfSameType = NULL;
		EEdGraphPinDirection DesiredDirection = FromPin->Direction == EGPD_Output ? EGPD_Input : EGPD_Output;
		for (UEdGraphPin* Pin : Pins)
		{
			//ENiagaraCompoundType ToType = Schema->GetPinDataType(Pin);
			if (/*FromType == ToType &&*/ Pin->Direction == DesiredDirection)
			{
				FirstPinOfSameType = Pin;
				break;
			}			
		}

		if (FirstPinOfSameType && GetSchema()->TryCreateConnection(FromPin, FirstPinOfSameType))
		{
			FromPin->GetOwningNode()->NodeConnectionListChanged();
		}
	}
}

bool UNiagaraNode::ConvertNumericPinToType(UEdGraphPin* InGraphPin, FNiagaraTypeDefinition TypeDef)
{
	int32 PinIndex = GetPinIndex(InGraphPin);
	if (PinIndex == -1)
	{
		return false;
	}

	const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
	check(Schema);
	FEdGraphPinType PinType = Schema->TypeDefinitionToPinType(TypeDef);
	if (PinType == InGraphPin->PinType)
	{
		return false;
	}

	InGraphPin->Modify();
	InGraphPin->PinType = PinType;
	InGraphPin->ResetDefaultValue();
	ReallocatePins();

	return true;
}

void UNiagaraNode::PinDefaultValueChanged(UEdGraphPin* Pin) 
{
	UNiagaraGraph* Graph = GetNiagaraGraph();
	Graph->NotifyGraphNeedsRecompile();
}

void UNiagaraNode::PinConnectionListChanged(UEdGraphPin* Pin) 
{
	UNiagaraGraph* Graph = GetNiagaraGraph();
	Graph->NotifyGraphNeedsRecompile();

	const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
	check(Schema);
	FNiagaraTypeDefinition TypeDef = Schema->PinToTypeDefinition(Pin);
	if (TypeDef == FNiagaraTypeDefinition::GetGenericNumericDef())
	{
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (LinkedPin != nullptr)
			{
				FNiagaraTypeDefinition LinkedPinTypeDef = Schema->PinToTypeDefinition(LinkedPin);
				if (LinkedPinTypeDef != FNiagaraTypeDefinition::GetGenericNumericDef())
				{

				}
			}
		}
	}
}

void UNiagaraNode::PinTypeChanged(UEdGraphPin* Pin) 
{
	UNiagaraGraph* Graph = GetNiagaraGraph();
	Graph->NotifyGraphNeedsRecompile();
}

const UNiagaraGraph* UNiagaraNode::GetNiagaraGraph()const
{
	return CastChecked<UNiagaraGraph>(GetGraph());
}

UNiagaraGraph* UNiagaraNode::GetNiagaraGraph()
{
	return CastChecked<UNiagaraGraph>(GetGraph());
}

UNiagaraScriptSource* UNiagaraNode::GetSource()const
{
	return GetNiagaraGraph()->GetSource();
}

UEdGraphPin* UNiagaraNode::GetInputPin(int32 InputIndex) const
{
	for (int32 PinIndex = 0, FoundInputs = 0; PinIndex < Pins.Num(); PinIndex++)
	{
		if (Pins[PinIndex]->Direction == EGPD_Input)
		{
			if (InputIndex == FoundInputs)
			{
				return Pins[PinIndex];
			}
			else
			{
				FoundInputs++;
			}
		}
	}

	return NULL;
}

bool UNiagaraNode::CanAddToGraph(UNiagaraGraph* TargetGraph, FString& OutErrorMsg) const
{
	if (TargetGraph == nullptr)
	{
		OutErrorMsg = LOCTEXT("NiagaraNodeInvalidGraph", "Target Graph is invalid.").ToString();
		return false;
	}

	return true;
}


void UNiagaraNode::GetInputPins(TArray<class UEdGraphPin*>& OutInputPins) const
{
	OutInputPins.Empty();

	for (int32 PinIndex = 0; PinIndex < Pins.Num(); PinIndex++)
	{
		if (Pins[PinIndex]->Direction == EGPD_Input)
		{
			OutInputPins.Add(Pins[PinIndex]);
		}
	}
}

UEdGraphPin* UNiagaraNode::GetOutputPin(int32 OutputIndex) const
{
	for (int32 PinIndex = 0, FoundOutputs = 0; PinIndex < Pins.Num(); PinIndex++)
	{
		if (Pins[PinIndex]->Direction == EGPD_Output)
		{
			if (OutputIndex == FoundOutputs)
			{
				return Pins[PinIndex];
			}
			else
			{
				FoundOutputs++;
			}
		}
	}

	return NULL;
}

void UNiagaraNode::GetOutputPins(TArray<class UEdGraphPin*>& OutOutputPins) const
{
	OutOutputPins.Empty();

	for (int32 PinIndex = 0; PinIndex < Pins.Num(); PinIndex++)
	{
		if (Pins[PinIndex]->Direction == EGPD_Output)
		{
			OutOutputPins.Add(Pins[PinIndex]);
		}
	}
}

ENiagaraNumericOutputTypeSelectionMode UNiagaraNode::GetNumericOutputTypeSelectionMode() const
{
	return ENiagaraNumericOutputTypeSelectionMode::None;
}

#undef LOCTEXT_NAMESPACE