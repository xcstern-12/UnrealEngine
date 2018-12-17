// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "SequencerKeyStructGenerator.h"
#include "Channels/MovieSceneChannelData.h"

UMovieSceneKeyStructType::UMovieSceneKeyStructType(const FObjectInitializer& ObjInit)
	: Super(ObjInit)
{
	SourceValuesProperty = nullptr;
	SourceTimesProperty  = nullptr;
	DestValueProperty    = nullptr;
	DestTimeProperty     = nullptr;
}

FSequencerKeyStructGenerator& FSequencerKeyStructGenerator::Get()
{
	static FSequencerKeyStructGenerator Instance;
	return Instance;
}

UMovieSceneKeyStructType* FSequencerKeyStructGenerator::AllocateNewKeyStruct()
{
	UMovieSceneKeyStructType* NewStruct = NewObject<UMovieSceneKeyStructType>(GetTransientPackage(), NAME_None, RF_Public | RF_Standalone);
	NewStruct->SetSuperStruct(FGeneratedMovieSceneKeyStruct::StaticStruct());
	return NewStruct;
}

UMovieSceneKeyStructType* FSequencerKeyStructGenerator::AllocateNewKeyStruct(UScriptStruct* ChannelType)
{
	static const FName TimesMetaDataTag("KeyTimes");
	static const FName ValuesMetaDataTag("KeyValues");

	UArrayProperty* SourceTimes  = FindArrayPropertyWithTag(ChannelType, TimesMetaDataTag);
	UArrayProperty* SourceValues = FindArrayPropertyWithTag(ChannelType, ValuesMetaDataTag);

	if (!ensureMsgf(SourceTimes, TEXT("No times property could be found for channel type %s. Please add KeyTimes meta data to the array containing the channel's key time."), *ChannelType->GetName()))
	{
		return nullptr;
	}
	else if (!ensureMsgf(SourceValues, TEXT("No value property could be found for channel type %s. Please add KeyValues meta data to the array containing the channel's key values."), *ChannelType->GetName()))
	{
		return nullptr;
	}

	UMovieSceneKeyStructType* NewStruct = AllocateNewKeyStruct();

	NewStruct->SourceTimesProperty  = SourceTimes;
	NewStruct->SourceValuesProperty = SourceValues;

	return NewStruct;
}

UMovieSceneKeyStructType* FSequencerKeyStructGenerator::DefaultInstanceGeneratedStruct(UScriptStruct* ChannelType)
{
	UMovieSceneKeyStructType* Existing = FindGeneratedStruct(ChannelType->GetFName());
	if (Existing)
	{
		return Existing;
	}
	else
	{
		UMovieSceneKeyStructType* NewStruct = FSequencerKeyStructGenerator::AllocateNewKeyStruct(ChannelType);
		if (!NewStruct)
		{
			return nullptr;
		}

		UProperty* NewValueProperty = DuplicateObject(NewStruct->SourceValuesProperty->Inner, NewStruct, "Value");
		NewValueProperty->SetPropertyFlags(CPF_Edit);
		NewValueProperty->SetMetaData("Category", TEXT("Key"));
		NewValueProperty->SetMetaData("ShowOnlyInnerProperties", TEXT("true"));
		NewValueProperty->ArrayDim = 1;

		NewStruct->AddCppProperty(NewValueProperty);
		NewStruct->DestValueProperty = NewValueProperty;

		FSequencerKeyStructGenerator::FinalizeNewKeyStruct(NewStruct);

		AddGeneratedStruct(ChannelType->GetFName(), NewStruct);
		return NewStruct;
	}
}

void FSequencerKeyStructGenerator::FinalizeNewKeyStruct(UMovieSceneKeyStructType* InStruct)
{
	check(InStruct);

	// Add the time property to the head of the property linked list (so it shows first)
	UStructProperty* NewTimeProperty = NewObject<UStructProperty>(InStruct, "Time");
	NewTimeProperty->SetPropertyFlags(CPF_Edit);
	NewTimeProperty->SetMetaData("Category", TEXT("Key"));
	NewTimeProperty->ArrayDim = 1;
	NewTimeProperty->Struct = TBaseStructure<FFrameNumber>::Get();

	InStruct->AddCppProperty(NewTimeProperty);
	InStruct->DestTimeProperty = NewTimeProperty;

	// Finalize the struct
	InStruct->Bind();
	InStruct->StaticLink(true);

	UMovieSceneKeyStructType::DeferCppStructOps(InStruct->GetFName(), new UScriptStruct::TCppStructOps<FGeneratedMovieSceneKeyStruct>);

	check(InStruct->IsComplete());
}

void FSequencerKeyStructGenerator::AddReferencedObjects( FReferenceCollector& Collector )
{
	Collector.AddReferencedObjects(InstanceNameToGeneratedStruct);
}

void FSequencerKeyStructGenerator::AddGeneratedStruct(FName InstancedStructName, UMovieSceneKeyStructType* Struct)
{
	check(!InstanceNameToGeneratedStruct.Contains(InstancedStructName));
	InstanceNameToGeneratedStruct.Add(InstancedStructName, Struct);
}

UMovieSceneKeyStructType* FSequencerKeyStructGenerator::FindGeneratedStruct(FName InstancedStructName)
{
	return InstanceNameToGeneratedStruct.FindRef(InstancedStructName);
}

UArrayProperty* FSequencerKeyStructGenerator::FindArrayPropertyWithTag(UScriptStruct* ChannelStruct, FName MetaDataTag)
{
	for (UArrayProperty* ArrayProperty : TFieldRange<UArrayProperty>(ChannelStruct))
	{
		if (ArrayProperty->HasMetaData(MetaDataTag))
		{
			return ArrayProperty;
		}
	}

	return nullptr;
}

TSharedPtr<FStructOnScope> FSequencerKeyStructGenerator::CreateInitialStructInstance(const void* SourceChannel, UMovieSceneKeyStructType* GeneratedStructType, int32 InitialKeyIndex)
{
	check(InitialKeyIndex != INDEX_NONE);

	TSharedPtr<FStructOnScope>     Struct    = MakeShared<FStructOnScope>(GeneratedStructType);
	FGeneratedMovieSceneKeyStruct* StructPtr = reinterpret_cast<FGeneratedMovieSceneKeyStruct*>(Struct->GetStructMemory());

	// Copy the initial time into the struct
	{

		const uint8* SrcTimeData  = GeneratedStructType->SourceTimesProperty->ContainerPtrToValuePtr<uint8>(SourceChannel);
		uint8*       DestTimeData = GeneratedStructType->DestTimeProperty->ContainerPtrToValuePtr<uint8>(StructPtr);

		FScriptArrayHelper SourceTimesArray(GeneratedStructType->SourceTimesProperty, SrcTimeData);
		GeneratedStructType->SourceTimesProperty->Inner->CopyCompleteValue(DestTimeData, SourceTimesArray.GetRawPtr(InitialKeyIndex));
	}

	// Copy the initial value into the struct
	{
		const uint8* SrcValueData  = GeneratedStructType->SourceValuesProperty->ContainerPtrToValuePtr<uint8>(SourceChannel);
		uint8*       DestValueData = GeneratedStructType->DestValueProperty->ContainerPtrToValuePtr<uint8>(StructPtr);

		FScriptArrayHelper SourceValuesArray(GeneratedStructType->SourceValuesProperty, SrcValueData);
		GeneratedStructType->SourceValuesProperty->Inner->CopyCompleteValue(DestValueData, SourceValuesArray.GetRawPtr(InitialKeyIndex));
	}

	return Struct;
}
