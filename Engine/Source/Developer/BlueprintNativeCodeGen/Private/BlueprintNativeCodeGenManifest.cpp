// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "BlueprintNativeCodeGenManifest.h"
#include "UObject/Package.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Engine/Blueprint.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "PlatformInfo.h"
#include "IBlueprintCompilerCppBackendModule.h"
#include "JsonObjectConverter.h"

DEFINE_LOG_CATEGORY_STATIC(LogNativeCodeGenManifest, Log, All);

/*******************************************************************************
 * BlueprintNativeCodeGenManifestImpl
 ******************************************************************************/

namespace BlueprintNativeCodeGenManifestImpl
{
	static const int64   CPF_NoFlags          = 0x00;
	static const FString ManifestFileExt      = TEXT(".BpCodeGenManifest.json");
	static const FString CppFileExt	          = TEXT(".cpp");
	static const FString HeaderFileExt        = TEXT(".h");
	static const FString HeaderSubDir         = TEXT("Public");
	static const FString CppSubDir            = TEXT("Private");
	static const FString ModuleBuildFileExt   = TEXT(".Build.cs");
	static const FString PreviewFilePostfix   = TEXT("-Preview");
	static const FString PluginFileExt        = TEXT(".uplugin");
	static const FString SourceSubDir         = TEXT("Source");
	static const FString EditorModulePostfix  = TEXT("Editor");
	static const int32   RootManifestId       = -1;

	/**
	 * Populates the provided manifest object with data from the specified file.
	 * 
	 * @param  FilePath    A json file path, denoting the file you want loaded and serialized in.
	 * @param  Manifest    The target object that you want filled out with data from the file.
	 * @return True if the manifest was successfully loaded, otherwise false.
	 */
	static bool LoadManifest(const FString& FilePath, FBlueprintNativeCodeGenManifest* Manifest);

	/**
	 * Helper function that homogenizes file/directory paths so that they can be 
	 * compared for equivalence against others.
	 * 
	 * @param  DirectoryPath    The path that you want sanitized.
	 * @return A equivalent file/directory path, standardized for comparisons.
	 */
	static FString GetComparibleDirPath(const FString& DirectoryPath);

	/**
	 * Retrieves the sub-directory for either header or cpp source files 
	 * (depending on which was requested).
	 * 
	 * @param  SourceType    Defines the type of source file to return for (header or cpp).
	 * @return A directory name for the specified source file type.
	 */
	static const FString& GetSourceSubDir(const FBlueprintNativeCodeGenPaths::ESourceFileType SourceType);

	/**
	 * Retrieves the extension for either header or cpp source files (depending
	 * on which was requested).
	 * 
	 * @param  SourceType    Defines the type of source file to return for (header or cpp).
	 * @return A file extension (including the leading dot), for the specified source file type.
	 */
	static const FString& GetSourceFileExt(const FBlueprintNativeCodeGenPaths::ESourceFileType SourceType);

	/**
	 * Constructs a source file path for the specified asset.
	 * 
	 * @param  TargetPaths	Specified the destination directory for the file.
	 * @param  Asset		The asset you want a header file for.
	 * @param  SourceType	Defines the type of source file to generate for (header or cpp).
	 * @return A target file path for the specified asset to save a header to.
	 */
	static FString GenerateSourceFileSavePath(const FBlueprintNativeCodeGenPaths& TargetPaths, const FAssetData& Asset, const FCompilerNativizationOptions& NativizationOptions, const FBlueprintNativeCodeGenPaths::ESourceFileType SourceType);

	/**
	 * 
	 * 
	 * @param  TargetPaths    
	 * @param  Asset    
	 * @return 
	 */
	static FString GenerateUnconvertedWrapperPath(const FBlueprintNativeCodeGenPaths& TargetPaths, const FAssetData& Asset, const FCompilerNativizationOptions& NativizationOptions);

	/**
	 * Coordinates with the code-gen backend, to produce a base filename (one 
	 * without a file extension).
	 * 
	 * @param  Asset    The asset you want a filename for.
	 * @return A filename (without extension) that matches the #include statements generated by the backend.
	 */
	static FString GetBaseFilename(const FAssetData& Asset, const FCompilerNativizationOptions& NativizationOptions);

	/**
	 * Collects native packages (which reflect distinct modules) that the 
	 * specified object relies upon.
	 * 
	 * @param  AssetObj			The object you want dependencies for.
	 * @param  DependenciesOut	An output array, which will be filled out with module packages that the target object relies upon.
	 * @return False if the function failed to collect dependencies for the specified object.
	 */
	static bool GatherModuleDependencies(const UObject* AssetObj, TArray<UPackage*>& DependenciesOut);

	/**
	 * Obtains the reflected name for the native field (class/enum/struct) that 
	 * we'll generate to replace the specified asset.
	 *
	 * @param  Asset	The asset you want a name from.
	 * @return The name of the asset field (class/enum/struct).
	 */
	static FString GetFieldName(const FAssetData& Asset);

	/**
	 * The object returned by FAssetData::GetAsset() doesn't always give us the 
	 * target object that will be replaced (for Blueprint's, it would be the 
	 * class instead). So this helper function will suss out the right object 
	 * for you.
	 * 
	 * @param  Asset    The asset you want an object for.
	 * @return A pointer to the targeted object from the asset's package.
	 */
	static UField* GetTargetAssetObject(const FAssetData& Asset);

	/**
	 * Returns the object path for the field from the specified asset's package
	 * that is being replaced (Asset.ObjectPath will not suffice, as that 
	 * does not always reflect the object that is being replaced).
	 * 
	 * @param  Asset    The asset you want an object-path for.
	 * @return An object-path for the target field-object within the asset's package.
	 */
	static FString GetTargetObjectPath(const FAssetData& Asset);
}

//------------------------------------------------------------------------------
static bool BlueprintNativeCodeGenManifestImpl::LoadManifest(const FString& FilePath, FBlueprintNativeCodeGenManifest* Manifest)
{
	FString ManifestStr;
	if (FFileHelper::LoadFileToString(ManifestStr, *FilePath))
	{
		TSharedRef< TJsonReader<> > JsonReader = TJsonReaderFactory<>::Create(ManifestStr);

		TSharedPtr<FJsonObject> JsonObject;
		if (FJsonSerializer::Deserialize(JsonReader, JsonObject))
		{
			return FJsonObjectConverter::JsonObjectToUStruct<FBlueprintNativeCodeGenManifest>(JsonObject.ToSharedRef(), Manifest,
				/*CheckFlags =*/CPF_NoFlags, /*SkipFlags =*/CPF_NoFlags);
		}
	}
	return false;
}

//------------------------------------------------------------------------------
static const FString& BlueprintNativeCodeGenManifestImpl::GetSourceSubDir(const FBlueprintNativeCodeGenPaths::ESourceFileType SourceType)
{
	return (SourceType == FBlueprintNativeCodeGenPaths::HFile) ? HeaderSubDir : CppSubDir;
}

//------------------------------------------------------------------------------
static const FString& BlueprintNativeCodeGenManifestImpl::GetSourceFileExt(const FBlueprintNativeCodeGenPaths::ESourceFileType SourceType)
{
	return (SourceType == FBlueprintNativeCodeGenPaths::HFile) ? HeaderFileExt : CppFileExt;
}

//------------------------------------------------------------------------------
static FString BlueprintNativeCodeGenManifestImpl::GenerateSourceFileSavePath(const FBlueprintNativeCodeGenPaths& TargetPaths, const FAssetData& Asset, const FCompilerNativizationOptions& NativizationOptions, const FBlueprintNativeCodeGenPaths::ESourceFileType SourceType)
{
	return FPaths::Combine(*TargetPaths.RuntimeSourceDir(SourceType), *GetBaseFilename(Asset, NativizationOptions)) + GetSourceFileExt(SourceType);
}

//------------------------------------------------------------------------------
static FString BlueprintNativeCodeGenManifestImpl::GenerateUnconvertedWrapperPath(const FBlueprintNativeCodeGenPaths& TargetPaths, const FAssetData& Asset, const FCompilerNativizationOptions& NativizationOptions)
{
	const FBlueprintNativeCodeGenPaths::ESourceFileType WrapperFileType = FBlueprintNativeCodeGenPaths::HFile;
	return FPaths::Combine(*TargetPaths.RuntimeSourceDir(WrapperFileType), *GetBaseFilename(Asset, NativizationOptions)) + GetSourceFileExt(WrapperFileType);
}

//------------------------------------------------------------------------------
static FString BlueprintNativeCodeGenManifestImpl::GetBaseFilename(const FAssetData& Asset, const FCompilerNativizationOptions& NativizationOptions)
{
	IBlueprintCompilerCppBackendModule& CodeGenBackend = (IBlueprintCompilerCppBackendModule&)IBlueprintCompilerCppBackendModule::Get();
	return CodeGenBackend.ConstructBaseFilename(Asset.GetAsset(), NativizationOptions);
}

//------------------------------------------------------------------------------
static FString BlueprintNativeCodeGenManifestImpl::GetComparibleDirPath(const FString& DirectoryPath)
{
	FString NormalizedPath = DirectoryPath;

	const FString PathDelim = TEXT("/");
	if (!NormalizedPath.EndsWith(PathDelim))
	{
		// to account for the case where the relative path would resolve to X: 
		// (when we want "X:/")... ConvertRelativePathToFull() leaves the 
		// trailing slash, and NormalizeDirectoryName() will remove it (if it is
		// not a drive letter)
		NormalizedPath += PathDelim;
	}

	if (FPaths::IsRelative(NormalizedPath))
	{
		NormalizedPath = FPaths::ConvertRelativePathToFull(NormalizedPath);
	}
	FPaths::NormalizeDirectoryName(NormalizedPath);
	return NormalizedPath;
}

//------------------------------------------------------------------------------
static bool BlueprintNativeCodeGenManifestImpl::GatherModuleDependencies(const UObject* AssetObj, TArray<UPackage*>& DependenciesOut)
{
	UPackage* AssetPackage = AssetObj->GetOutermost();

	const FLinkerLoad* PkgLinker = FLinkerLoad::FindExistingLinkerForPackage(AssetPackage);
	const bool bFoundLinker = (PkgLinker != nullptr);

	if (ensureMsgf(bFoundLinker, TEXT("Failed to identify the asset package that '%s' belongs to."), *AssetObj->GetName()))
	{
		for (const FObjectImport& PkgImport : PkgLinker->ImportMap)
		{
			if (PkgImport.ClassName != NAME_Package)
			{
				continue;
			}

			UPackage* DependentPackage = FindObject<UPackage>(/*Outer =*/nullptr, *PkgImport.ObjectName.ToString(), /*ExactClass =*/true);
			if (DependentPackage == nullptr)
			{
				continue;
			}

			// we want only native packages, ones that are not editor-only
			if ((DependentPackage->GetPackageFlags() & (PKG_CompiledIn | PKG_EditorOnly | PKG_Developer)) == PKG_CompiledIn)
			{
				DependenciesOut.AddUnique(DependentPackage);// PkgImport.ObjectName.ToString());
			}
		}
	}

	return bFoundLinker;
}

//------------------------------------------------------------------------------
static FString BlueprintNativeCodeGenManifestImpl::GetFieldName(const FAssetData& Asset)
{
	UField* AssetField = GetTargetAssetObject(Asset);
	return (AssetField != nullptr) ? AssetField->GetName() : TEXT("");
}

//------------------------------------------------------------------------------
static UField* BlueprintNativeCodeGenManifestImpl::GetTargetAssetObject(const FAssetData& Asset)
{
	UObject* AssetObj = Asset.GetAsset();

	UField* AssetField = nullptr;
	if (UBlueprint* BlueprintAsset = Cast<UBlueprint>(AssetObj))
	{
		AssetField = BlueprintAsset->GeneratedClass;
		if (!AssetField)
		{
			UE_LOG(LogNativeCodeGenManifest, Warning, TEXT("null BPGC in %s"), *BlueprintAsset->GetPathName());
		}
	}
	else
	{
		// only other asset types that we should be converting are enums and 
		// structs (both UFields)
		AssetField = CastChecked<UField>(AssetObj);
	}

	return AssetField;
}

//------------------------------------------------------------------------------
static FString BlueprintNativeCodeGenManifestImpl::GetTargetObjectPath(const FAssetData& Asset)
{
	UField* AssetField = GetTargetAssetObject(Asset);
	return (AssetField != nullptr) ? AssetField->GetPathName() : TEXT("");
}

/*******************************************************************************
 * FConvertedAssetRecord
 ******************************************************************************/
 
//------------------------------------------------------------------------------
FConvertedAssetRecord::FConvertedAssetRecord(const FAssetData& AssetInfo, const FBlueprintNativeCodeGenPaths& TargetPaths, const FCompilerNativizationOptions& NativizationOptions)
	: AssetType(AssetInfo.GetClass())
	, TargetObjPath(BlueprintNativeCodeGenManifestImpl::GetTargetObjectPath(AssetInfo))
{
	GeneratedCppPath    = BlueprintNativeCodeGenManifestImpl::GenerateSourceFileSavePath(TargetPaths, AssetInfo, NativizationOptions, FBlueprintNativeCodeGenPaths::CppFile);
	GeneratedHeaderPath = BlueprintNativeCodeGenManifestImpl::GenerateSourceFileSavePath(TargetPaths, AssetInfo, NativizationOptions, FBlueprintNativeCodeGenPaths::HFile);
}

/*******************************************************************************
 * FUnconvertedDependencyRecord
 ******************************************************************************/
 
//------------------------------------------------------------------------------
FUnconvertedDependencyRecord::FUnconvertedDependencyRecord(const FString& InGeneratedWrapperPath)
	: GeneratedWrapperPath(InGeneratedWrapperPath)
{
}

/*******************************************************************************
 * FBlueprintNativeCodeGenPaths
 ******************************************************************************/

 //------------------------------------------------------------------------------
FBlueprintNativeCodeGenPaths FBlueprintNativeCodeGenPaths::GetDefaultCodeGenPaths(const FName PlatformName)
{
	static const FString DefaultPluginName = TEXT("NativizedAssets");

	FString DefaultPluginPath = FPaths::Combine(*FPaths::ProjectIntermediateDir(), TEXT("Plugins"), *DefaultPluginName);
	if (!PlatformName.IsNone())
	{
		for (PlatformInfo::FPlatformEnumerator PlatformIt = PlatformInfo::EnumeratePlatformInfoArray(); PlatformIt; ++PlatformIt)
		{
			if (PlatformIt->TargetPlatformName == PlatformName)
			{
				static const FName UBTTargetId_Win32 = FName(TEXT("Win32"));
				static const FName UBTTargetId_Win64 = FName(TEXT("Win64"));
				static const FName UBTTargetId_Windows = FName(TEXT("Windows"));

				const FName UBTTargetId = (PlatformIt->UBTTargetId == UBTTargetId_Win32 || PlatformIt->UBTTargetId == UBTTargetId_Win64) ? UBTTargetId_Windows : PlatformIt->UBTTargetId;
				DefaultPluginPath = FPaths::Combine(*DefaultPluginPath, *LexToString(UBTTargetId), *LexToString(PlatformIt->PlatformType));
				break;
			}
		}
	}

	return FBlueprintNativeCodeGenPaths(DefaultPluginName, DefaultPluginPath, PlatformName);
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::GetDefaultPluginPath(const FName PlatformName)
{
	return GetDefaultCodeGenPaths(PlatformName).PluginFilePath();
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::GetDefaultManifestFilePath(const FName PlatformName, const int32 ChunkId)
{
	return GetDefaultCodeGenPaths(PlatformName).ManifestFilename(ChunkId);
}

//------------------------------------------------------------------------------
FBlueprintNativeCodeGenPaths::FBlueprintNativeCodeGenPaths(const FString& PluginNameIn, const FString& TargetDirIn, const FName PlatformNameIn)
	: PluginsDir(TargetDirIn)
	, PluginName(PluginNameIn)
	, PlatformName(PlatformNameIn)
{
	// PluginsDir is expected to be the generic 'Plugins' directory (we'll add our own subfolder matching the plugin name)
	if (FPaths::GetBaseFilename(PluginsDir) == PluginNameIn)
	{
		PluginsDir = FPaths::GetPath(PluginsDir);
	}
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::ManifestFilename(const int32 ChunkId) const
{
	using namespace BlueprintNativeCodeGenManifestImpl;

	FString Filename = FApp::GetProjectName() + (TEXT("_") + PlatformName.ToString());
	if (ChunkId != RootManifestId)
	{
		Filename += FString::Printf(TEXT("-%02d"), ChunkId);
	}
	return Filename + ManifestFileExt;
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::ManifestFilePath(const int32 ChunkId) const
{
	return FPaths::Combine(*RuntimeModuleDir(), *ManifestFilename(ChunkId));
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::PluginRootDir() const
{
	return PluginsDir;
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::PluginFilePath() const
{
	return FPaths::Combine(*PluginRootDir(), *PluginName) + BlueprintNativeCodeGenManifestImpl::PluginFileExt;
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::PluginSourceDir() const
{
	return FPaths::Combine(*PluginRootDir(), *BlueprintNativeCodeGenManifestImpl::SourceSubDir);
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::RuntimeModuleDir() const
{
	return FPaths::Combine(*PluginSourceDir(), *RuntimeModuleName());
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::RuntimeModuleName() const
{
	FString ModuleName = PluginName;
// 	if (!PlatformName.IsNone())
// 	{
// 		ModuleName += TEXT("_") + PlatformName.ToString();
// 	}
	return ModuleName;
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::RuntimeBuildFile() const
{
	return FPaths::Combine(*RuntimeModuleDir(), *RuntimeModuleName()) + BlueprintNativeCodeGenManifestImpl::ModuleBuildFileExt;
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::RuntimeSourceDir(ESourceFileType SourceType) const
{
	return FPaths::Combine(*RuntimeModuleDir(), *BlueprintNativeCodeGenManifestImpl::GetSourceSubDir(SourceType));
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::RuntimeModuleFile(ESourceFileType SourceType) const
{
	// use the "cpp" (private) directory for the header too (which acts as a PCH)
	return FPaths::Combine(*RuntimeSourceDir(ESourceFileType::CppFile), *RuntimeModuleName()) + BlueprintNativeCodeGenManifestImpl::GetSourceFileExt(SourceType);
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenPaths::RuntimePCHFilename() const
{
	return FPaths::GetCleanFilename(RuntimeModuleFile(HFile));
}

/*******************************************************************************
 * FBlueprintNativeCodeGenManifest
 ******************************************************************************/

//------------------------------------------------------------------------------
FBlueprintNativeCodeGenManifest::FBlueprintNativeCodeGenManifest(int32 ManifestId)
	: ManifestChunkId(ManifestId)
{
	InitDestPaths(FBlueprintNativeCodeGenPaths::GetDefaultPluginPath(NAME_None));
}

//------------------------------------------------------------------------------
FBlueprintNativeCodeGenManifest::FBlueprintNativeCodeGenManifest(const FCompilerNativizationOptions& InCompilerNativizationOptions, int32 ManifestId)
	: ManifestChunkId(ManifestId)
	, NativizationOptions(InCompilerNativizationOptions)
{
	InitDestPaths(FBlueprintNativeCodeGenPaths::GetDefaultPluginPath(InCompilerNativizationOptions.PlatformName));
}

FBlueprintNativeCodeGenManifest::FBlueprintNativeCodeGenManifest(const FString& PluginPath, const FCompilerNativizationOptions& InCompilerNativizationOptions, int32 ManifestId)
	: ManifestChunkId(ManifestId)
	, NativizationOptions(InCompilerNativizationOptions)
{
	InitDestPaths(PluginPath);
}

//------------------------------------------------------------------------------
FBlueprintNativeCodeGenManifest::FBlueprintNativeCodeGenManifest(const FString& ManifestFilePathIn)
{
	ensureAlwaysMsgf( BlueprintNativeCodeGenManifestImpl::LoadManifest(ManifestFilePathIn, this), TEXT("Missing Manifest for Blueprint code generation: %s"), *ManifestFilePathIn);
}

//------------------------------------------------------------------------------
FBlueprintNativeCodeGenPaths FBlueprintNativeCodeGenManifest::GetTargetPaths() const
{
	return FBlueprintNativeCodeGenPaths(PluginName, GetTargetDir(), NativizationOptions.PlatformName);
}

//------------------------------------------------------------------------------
FConvertedAssetRecord& FBlueprintNativeCodeGenManifest::CreateConversionRecord(const FAssetId Key, const FAssetData& AssetInfo)
{
	// It is an error to consider a record converted and unconverted. This is probably not negotiable. In order to leave the door
	// open for parallelism we need to be able to trivially and consistently determine whether an asset will be converted without
	// discovering things as we go.
	check(UnconvertedDependencies.Find(Key) == nullptr);
	// Similarly we should not convert things over and over and over:
	if (FConvertedAssetRecord* Existing = ConvertedAssets.Find(Key))
	{
		return *Existing;
	}

	const FBlueprintNativeCodeGenPaths TargetPaths = GetTargetPaths();
	UClass* AssetType = AssetInfo.GetClass();
	// load the asset (if it isn't already)
	const UObject* AssetObj = AssetInfo.GetAsset();
	FConvertedAssetRecord* ConversionRecord = &ConvertedAssets.Add(Key, FConvertedAssetRecord(AssetInfo, TargetPaths, NativizationOptions));
	return *ConversionRecord;
}

//------------------------------------------------------------------------------
FUnconvertedDependencyRecord& FBlueprintNativeCodeGenManifest::CreateUnconvertedDependencyRecord(const FAssetId UnconvertedAssetKey, const FAssetData& AssetInfo)
{
	// It is an error to create a record multiple times because it is not obvious what the client wants to happen.
	check(ConvertedAssets.Find(UnconvertedAssetKey) == nullptr);

	if (FUnconvertedDependencyRecord* Existing = UnconvertedDependencies.Find(UnconvertedAssetKey))
	{
		return *Existing;
	}

	const FBlueprintNativeCodeGenPaths TargetPaths = GetTargetPaths();
	FUnconvertedDependencyRecord* RecordPtr = &UnconvertedDependencies.Add(UnconvertedAssetKey, FUnconvertedDependencyRecord(BlueprintNativeCodeGenManifestImpl::GenerateUnconvertedWrapperPath(TargetPaths, AssetInfo, NativizationOptions)));
	return *RecordPtr;
}

//------------------------------------------------------------------------------
void FBlueprintNativeCodeGenManifest::GatherModuleDependencies(UPackage* Package)
{
	BlueprintNativeCodeGenManifestImpl::GatherModuleDependencies(Package, ModuleDependencies);
}

//------------------------------------------------------------------------------
void FBlueprintNativeCodeGenManifest::AddSingleModuleDependency(UPackage* Package)
{
	ModuleDependencies.AddUnique(Package);
}

//------------------------------------------------------------------------------
bool FBlueprintNativeCodeGenManifest::Save() const
{
	const FString FullFilename = GetTargetPaths().ManifestFilePath(ManifestChunkId);	

	TSharedRef<FJsonObject> JsonObject = MakeShareable(new FJsonObject());

	if (FJsonObjectConverter::UStructToJsonObject(FBlueprintNativeCodeGenManifest::StaticStruct(), this, JsonObject,
		/*CheckFlags =*/BlueprintNativeCodeGenManifestImpl::CPF_NoFlags, /*SkipFlags =*/BlueprintNativeCodeGenManifestImpl::CPF_NoFlags))
	{
		FString FileContents;
		TSharedRef< TJsonWriter<> > JsonWriter = TJsonWriterFactory<>::Create(&FileContents);

		if (FJsonSerializer::Serialize(JsonObject, JsonWriter))
		{
			JsonWriter->Close();
			return FFileHelper::SaveStringToFile(FileContents, *FullFilename);
		}
	}
	return false;
}

void FBlueprintNativeCodeGenManifest::Merge(const FBlueprintNativeCodeGenManifest& OtherManifest)
{
	for (auto& Entry : OtherManifest.ModuleDependencies)
	{
		ModuleDependencies.AddUnique(Entry);
	}

	for (auto& Entry : OtherManifest.ConvertedAssets)
	{
		ConvertedAssets.Add(Entry.Key, Entry.Value);
	}

	for (auto& Entry : OtherManifest.UnconvertedDependencies)
	{
		UnconvertedDependencies.Add(Entry.Key, Entry.Value);
	}
}

//------------------------------------------------------------------------------
void FBlueprintNativeCodeGenManifest::InitDestPaths(const FString& PluginPath)
{
	using namespace BlueprintNativeCodeGenManifestImpl;

	if (ensure(!PluginPath.IsEmpty()))
	{
		PluginName = FPaths::GetBaseFilename(PluginPath);

		if (ensure(PluginPath.EndsWith(PluginFileExt)))
		{
			OutputDir = FPaths::GetPath(PluginPath);
		}
		else
		{
			OutputDir = PluginPath;
		}

		if (FPaths::IsRelative(OutputDir))
		{
			FPaths::MakePathRelativeTo(OutputDir, *FPaths::ProjectDir());
		}
	}
	else
	{
		FBlueprintNativeCodeGenPaths DefaultPaths =  FBlueprintNativeCodeGenPaths::GetDefaultCodeGenPaths(NAME_None);
		PluginName = DefaultPaths.GetPluginName();
		OutputDir  = DefaultPaths.PluginRootDir();
	}

	// there's some sanitation that FBlueprintNativeCodeGenPaths does that we 
	// can avoid in the future, if we pass this through it once
	OutputDir = GetTargetPaths().PluginRootDir();
}

//------------------------------------------------------------------------------
FString FBlueprintNativeCodeGenManifest::GetTargetDir() const
{
	FString TargetPath = OutputDir;
	if (FPaths::IsRelative(TargetPath))
	{
		TargetPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), TargetPath);
		TargetPath = FPaths::ConvertRelativePathToFull(TargetPath);
	}
	return TargetPath;
}

//------------------------------------------------------------------------------
void FBlueprintNativeCodeGenManifest::Clear()
{
	ConvertedAssets.Empty();
}
