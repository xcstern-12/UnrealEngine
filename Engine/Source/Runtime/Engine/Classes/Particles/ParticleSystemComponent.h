// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.


#pragma once
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h"
#include "Particles/Emitter.h"
#include "Particles/ParticleSystem.h"
#include "ParticleSystemComponent.generated.h"

//
// Forward declarations.
//
class UAnimNotifyState;
class FParticleDynamicData;
class FParticleSystemSceneProxy;
class UAnimNotifyState;
struct FDynamicEmitterDataBase;
struct FDynamicEmitterReplayDataBase;
struct FParticleAnimTrailEmitterInstance;

/** Enum for specifying type of a name instance parameter. */
UENUM(BlueprintType)
enum EParticleSysParamType
{
	PSPT_None UMETA(DisplayName="None"),
	PSPT_Scalar UMETA(DisplayName="Scalar"),
	PSPT_ScalarRand UMETA(DisplayName="Scalar Random"),
	PSPT_Vector UMETA(DisplayName="Vector"),
	PSPT_VectorRand UMETA(DisplayName="Vector Random"),
	PSPT_Color UMETA(DisplayName="Color"),
	PSPT_Actor UMETA(DisplayName="Actor"),
	PSPT_Material UMETA(DisplayName="Material"),
	PSPT_MAX,
};

/** Particle system replay state */
UENUM()
enum ParticleReplayState
{
	/** Replay system is disabled.  Particles are simulated and rendered normally. */
	PRS_Disabled UMETA(DisplayName="Disabled"),
	/** Capture all particle data to the clip specified by ReplayClipIDNumber.  The frame to capture
		must be specified using the ReplayFrameIndex */
	PRS_Capturing UMETA(DisplayName="Capturing"),
	/** Replay captured particle state from the clip specified by ReplayClipIDNumber.  The frame to play
		must be specified using the ReplayFrameIndex */
	PRS_Replaying UMETA(DisplayName="Replaying"),
	PRS_MAX,
};

/**
 *	Event type
 */
UENUM()
enum EParticleEventType
{
	/** Any - allow any event */
	EPET_Any UMETA(DisplayName="Any"),
	/** Spawn - a particle spawn event */
	EPET_Spawn UMETA(DisplayName="Spawn"),
	/** Death - a particle death event */
	EPET_Death UMETA(DisplayName="Death"),
	/** Collision - a particle collision event */
	EPET_Collision UMETA(DisplayName="Collision"),
	/** Burst - a particle burst event */
	EPET_Burst UMETA(DisplayName="Burst"),
	/** Blueprint - an event generated by level script */
	EPET_Blueprint UMETA(DisplayName="Blueprint"),
	EPET_MAX,
};

// Called when the particle system is done
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam( FOnSystemFinished, class UParticleSystemComponent*, PSystem );


/** Struct used for a particular named instance parameter for this ParticleSystemComponent. */
USTRUCT(BlueprintType)
struct FParticleSysParam
{
	GENERATED_USTRUCT_BODY()

	/** The name of the parameter */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	FName Name;

	/**
	 *	The type of parameters
	 *	PSPT_None       - There is no data type
	 *	PSPT_Scalar     - Use the scalar value
	 *	PSPT_ScalarRand - Select a scalar value in the range [Scalar_Low..Scalar)
	 *	PSPT_Vector     - Use the vector value
	 *	PSPT_VectorRand - Select a vector value in the range [Vector_Low..Vector)
	 *	PSPT_Color      - Use the color value
	 *	PSPT_Actor      - Use the actor value
	 *	PSPT_Material   - Use the material value
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	TEnumAsByte<enum EParticleSysParamType> ParamType;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	float Scalar;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	float Scalar_Low;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	FVector Vector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	FVector Vector_Low;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	FColor Color;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	class AActor* Actor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParticleSysParam)
	class UMaterialInterface* Material;


	FParticleSysParam()
		: ParamType(0)
		, Scalar(0)
		, Scalar_Low(0)
		, Vector(ForceInit)
		, Vector_Low(ForceInit)
		, Color(ForceInit)
		, Actor(NULL)
		, Material(NULL)
	{
	}

};

/**
 *	The base class for all particle event data.
 */
struct FParticleEventData
{
	/** The type of event that was generated. */
	int32 Type;

	/** The name of the event. */
	FName EventName;

	/** The emitter time at the event. */
	float EmitterTime;

	/** The location of the event. */
	FVector Location;

	/** The velocity at the time of the event. */
	FVector Velocity;

	/** Game specific event metadata */
	TArray<class UParticleModuleEventSendToGame*> EventData;

	FParticleEventData()
		: Type(0)
		, EmitterTime(0)
	{
	}
};

/**
 *	Particle event data for particles that already existed at the time of the event
 */
struct FParticleExistingData : FParticleEventData
{
	/** How long the particle had been alive at the time of the event. */
	float ParticleTime;

	/** The direction of the particle at the time of the event. */
	FVector Direction;

	FParticleExistingData()
		: ParticleTime(0)
		, Direction(ForceInit)
	{
	}
};

/**
 *	Spawn particle event data.
 */
struct FParticleEventSpawnData : public FParticleEventData
{
};

/**
 *	Killed particle event data.
 */
struct FParticleEventDeathData : public FParticleExistingData
{

};

/**
 *	Collision particle event data.
 */
struct FParticleEventCollideData : public FParticleExistingData
{
	/** Normal vector in coordinate system of the returner. Zero=none. */
	FVector Normal;

	/** Time until hit, if line check. */
	float Time;

	/** Primitive data item which was hit, INDEX_NONE=none. */
	int32 Item;

	/** Name of bone we hit (for skeletal meshes). */
	FName BoneName;


	FParticleEventCollideData()
		: Normal(ForceInit)
		, Time(0)
		, Item(0)
	{
	}

};

/**
 *	Particle burst event data.
 */
struct FParticleEventBurstData : public FParticleEventData
{
	int32 ParticleCount;

	FParticleEventBurstData()
		: ParticleCount(0)
	{
	}
};

/**
 *	Kismet particle event data.
 */
struct FParticleEventKismetData : public FParticleEventData
{
};

/** 
 * A particle emitter.
 */
UCLASS(ClassGroup=Rendering, hidecategories=Object, hidecategories=Physics, hidecategories=Collision, showcategories=Trigger, editinlinenew, meta=(BlueprintSpawnableComponent))
class ENGINE_API UParticleSystemComponent : public UPrimitiveComponent
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Particles)
	class UParticleSystem* Template;

	UPROPERTY(transient, duplicatetransient)
	TArray<class UMaterialInterface*> EmitterMaterials;

	/**
	 *	The skeletal mesh components used with the socket location module.
	 *	This is to prevent them from being garbage collected.
	 */
	UPROPERTY(transient, duplicatetransient)
	TArray<class USkeletalMeshComponent*> SkelMeshComponents;

	uint32 bWasCompleted:1;

	uint32 bSuppressSpawning:1;

private:
	/** if true, this psys can tick in any thread **/
	uint32 bIsElligibleForAsyncTick:1;
	/** if true, bIsElligibleForAsyncTick is set up **/
	uint32 bIsElligibleForAsyncTickComputed:1;
public:

	uint32 bWasDeactivated:1;

	/** True if this was active before being unregistered or otherwise reset, if so reactivate it */
	uint32 bWasActive:1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Particles)
	uint32 bResetOnDetach:1;

	/** whether to update the particle system on dedicated servers */
	UPROPERTY()
	uint32 bUpdateOnDedicatedServer:1;

	/** Indicates that the component has not been ticked since being registered. */
	uint32 bJustRegistered:1;

	/** This flag will be set the first time the PSysComp is activated... used to prevent auto activated PSysComps from calling InitParticles twice on level load */
	uint32 bHasBeenActivated:1;

	/**
	 *	Array holding name instance parameters for this ParticleSystemComponent.
	 *	Parameters can be used in Cascade using DistributionFloat/VectorParticleParameters.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Particles)
	TArray<struct FParticleSysParam> InstanceParameters;

	UPROPERTY(BlueprintAssignable)
	FParticleSpawnSignature OnParticleSpawn;

	UPROPERTY(BlueprintAssignable)
	FParticleBurstSignature OnParticleBurst;

	UPROPERTY(BlueprintAssignable)
	FParticleDeathSignature OnParticleDeath;

	UPROPERTY(BlueprintAssignable)
	FParticleCollisionSignature OnParticleCollide;

	UPROPERTY()
	FVector OldPosition;

	UPROPERTY()
	FVector PartSysVelocity;

	UPROPERTY()
	float WarmupTime;

	UPROPERTY()
	float WarmupTickRate;

	UPROPERTY()
	uint32 bWarmingUp:1;

private:
	int32 LODLevel;

public:

	uint32 bAutoDestroy:1;

	/**
	 * Number of seconds of emitter not being rendered that need to pass before it
	 * no longer gets ticked/ becomes inactive.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Particles)
	float SecondsBeforeInactive;

private:
	/** Tracks the time since the last forced UpdateTransform. */
	float TimeSinceLastForceUpdateTransform;

	/** Indicates if the component's transform has been changed and requires updating */
	bool bIsTransformDirty;

public:
	/**
	 * Time between forced UpdateTransforms for systems that use dynamically calculated bounds,
	 * Which is effectively how often the bounds are shrunk.
	 */
	UPROPERTY()
	float MaxTimeBeforeForceUpdateTransform;

#if WITH_EDITORONLY_DATA
	/**
	 *	INTERNAL. Used by the editor to set the LODLevel
	 */
	UPROPERTY()
	int32 EditorLODLevel;

	/**
	 * Used for applying Cascade's detail mode setting to in-level particle systems
	 */
	UPROPERTY()
	int32 EditorDetailMode;

#endif // WITH_EDITORONLY_DATA
	/** Used to accumulate total tick time to determine whether system can be skipped ticking if not visible. */
	float AccumTickTime;

	/** indicates that the component's LODMethod overrides the Template's */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LOD)
	uint32 bOverrideLODMethod:1;

	/** The method of LOD level determination to utilize for this particle system */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LOD)
	TEnumAsByte<enum ParticleSystemLODMethod> LODMethod;

	/**
	 *	Flag indicating that dynamic updating of render data should NOT occur during Tick.
	 *	This is used primarily to allow for warming up and simulated effects to a certain state.
	 */
	UPROPERTY()
	uint32 bSkipUpdateDynamicDataDuringTick:1;

	/** This is set when any of our "don't tick me" timeout values have fired */
	uint32 bForcedInActive:1;

	/** If true, force an LOD update from the renderer. */
	uint32 bForceLODUpdateFromRenderer:1;

	/** The view relevance flags for each LODLevel. */
	UPROPERTY(transient)
	TArray<FMaterialRelevance> CachedViewRelevanceFlags;

	/** If true, the ViewRelevanceFlags are dirty and should be recached */
	uint32 bIsViewRelevanceDirty:1;

	/** Array of replay clips for this particle system component.  These are serialized to disk.  You really should never add anything to this in the editor.  It's exposed so that you can delete clips if you need to, but be careful when doing so! */
	UPROPERTY()
	TArray<class UParticleSystemReplay*> ReplayClips;

	/** Current particle 'replay state'.  This setting controls whether we're currently simulating/rendering particles normally, or whether we should capture or playback particle replay data instead. */
	TEnumAsByte<enum ParticleReplayState> ReplayState;

	/** Clip ID number we're either playing back or capturing to, depending on the value of ReplayState. */
	int32 ReplayClipIDNumber;

	/** The current replay frame for playback */
	int32 ReplayFrameIndex;

	/** LOD updating... */
	float AccumLODDistanceCheckTime;

	/** The Spawn events that occurred in this PSysComp. */
	TArray<struct FParticleEventSpawnData> SpawnEvents;

	/** The Death events that occurred in this PSysComp. */
	TArray<struct FParticleEventDeathData> DeathEvents;

	/** The Collision events that occurred in this PSysComp. */
	TArray<struct FParticleEventCollideData> CollisionEvents;

	/** The Burst events that occurred in this PSysComp. */
	TArray<struct FParticleEventBurstData> BurstEvents;

	/** The Kismet events that occurred for this PSysComp. */
	TArray<struct FParticleEventKismetData> KismetEvents;

	/** Scales DeltaTime in UParticleSystemComponent::Tick(...) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Particles)
	float CustomTimeDilation;

	/** This is created at start up and then added to each emitter */
	float EmitterDelay;

	// Called when the particle system is done
	UPROPERTY(BlueprintAssignable)
	FOnSystemFinished OnSystemFinished;

	//
	//	Beam-related script functions.
	//
	
	/**
	 *	Set the beam end point
	 *
	 *	@param	EmitterIndex		The index of the emitter to set it on
	 *	@param	NewEndPoint			The value to set it to
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	virtual void SetBeamEndPoint(int32 EmitterIndex, FVector NewEndPoint);

	/**
	 *	Set the beam source point
	 *
	 *	@param	EmitterIndex		The index of the emitter to set it on
	 *	@param	NewSourcePoint		The value to set it to
	 *	@param	SourceIndex			Which beam within the emitter to set it on
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	virtual void SetBeamSourcePoint(int32 EmitterIndex, FVector NewSourcePoint, int32 SourceIndex);

	/**
	 *	Set the beam source tangent
	 *
	 *	@param	EmitterIndex		The index of the emitter to set it on
	 *	@param	NewTangentPoint		The value to set it to
	 *	@param	SourceIndex			Which beam within the emitter to set it on
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	virtual void SetBeamSourceTangent(int32 EmitterIndex, FVector NewTangentPoint, int32 SourceIndex);

	/**
	 *	Set the beam source strength
	 *
	 *	@param	EmitterIndex		The index of the emitter to set it on
	 *	@param	NewSourceStrength	The value to set it to
	 *	@param	SourceIndex			Which beam within the emitter to set it on
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	virtual void SetBeamSourceStrength(int32 EmitterIndex, float NewSourceStrength, int32 SourceIndex);

	/**
	 *	Set the beam target point
	 *
	 *	@param	EmitterIndex		The index of the emitter to set it on
	 *	@param	NewTargetPoint		The value to set it to
	 *	@param	TargetIndex			Which beam within the emitter to set it on
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	virtual void SetBeamTargetPoint(int32 EmitterIndex, FVector NewTargetPoint, int32 TargetIndex);

	/**
	 *	Set the beam target tangent
	 *
	 *	@param	EmitterIndex		The index of the emitter to set it on
	 *	@param	NewTangentPoint		The value to set it to
	 *	@param	TargetIndex			Which beam within the emitter to set it on
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	virtual void SetBeamTargetTangent(int32 EmitterIndex, FVector NewTangentPoint, int32 TargetIndex);

	/**
	 *	Set the beam target strength
	 *
	 *	@param	EmitterIndex		The index of the emitter to set it on
	 *	@param	NewTargetStrength	The value to set it to
	 *	@param	TargetIndex			Which beam within the emitter to set it on
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	virtual void SetBeamTargetStrength(int32 EmitterIndex, float NewTargetStrength, int32 TargetIndex);

	/**
	 *	Enables/Disables a sub-emitter
	 *
	 *	@param	EmitterName			The name of the sub-emitter to set it on
	 *	@param	bNewEnableState		The value to set it to
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	virtual void SetEmitterEnable(FName EmitterName, bool bNewEnableState);

	/** Change a named float parameter */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	void SetFloatParameter(FName ParameterName, float Param);

	/** 
	 *	Set a named vector instance parameter on this ParticleSystemComponent. 
	 *	Updates the parameter if it already exists, or creates a new entry if not. 
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	void SetVectorParameter(FName ParameterName, FVector Param);

	/** 
	 *	Set a named color instance parameter on this ParticleSystemComponent. 
	 *	Updates the parameter if it already exists, or creates a new entry if not. 
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	void SetColorParameter(FName ParameterName, FLinearColor Param);

	/** 
	 *	Set a named actor instance parameter on this ParticleSystemComponent. 
	 *	Updates the parameter if it already exists, or creates a new entry if not. 
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	void SetActorParameter(FName ParameterName, class AActor* Param);

	/** 
	 *	Set a named material instance parameter on this ParticleSystemComponent. 
	 *	Updates the parameter if it already exists, or creates a new entry if not. 
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	void SetMaterialParameter(FName ParameterName, class UMaterialInterface* Param);

	/**
	 *	Retrieve the Float parameter value for the given name.
	 *
	 *	@param	InName		Name of the parameter
	 *	@param	OutFloat	The value of the parameter found
	 *
	 *	@return	true		Parameter was found - OutFloat is valid
	 *			false		Parameter was not found - OutFloat is invalid
	 */
	virtual bool GetFloatParameter(const FName InName, float& OutFloat);

	/**
	 *	Retrieve the Vector parameter value for the given name.
	 *
	 *	@param	InName		Name of the parameter
	 *	@param	OutVector	The value of the parameter found
	 *
	 *	@return	true		Parameter was found - OutVector is valid
	 *			false		Parameter was not found - OutVector is invalid
	 */
	virtual bool GetVectorParameter(const FName InName, FVector& OutVector);

	/**
	 *	Retrieve the Color parameter value for the given name.
	 *
	 *	@param	InName		Name of the parameter
	 *	@param	OutColor	The value of the parameter found
	 *
	 *	@return	true		Parameter was found - OutColor is valid
	 *			false		Parameter was not found - OutColor is invalid
	 */
	virtual bool GetColorParameter(const FName InName, FLinearColor& OutColor);

	/**
	 *	Retrieve the Actor parameter value for the given name.
	 *
	 *	@param	InName		Name of the parameter
	 *	@param	OutActor	The value of the parameter found
	 *
	 *	@return	true		Parameter was found - OutActor is valid
	 *			false		Parameter was not found - OutActor is invalid
	 */
	virtual bool GetActorParameter(const FName InName, class AActor*& OutActor);

	/**
	 *	Retrieve the Material parameter value for the given name.
	 *
	 *	@param	InName		Name of the parameter
	 *	@param	OutMaterial	The value of the parameter found
	 *
	 *	@return	true		Parameter was found - OutMaterial is valid
	 *			false		Parameter was not found - OutMaterial is invalid
	 */
	virtual bool GetMaterialParameter(const FName InName, class UMaterialInterface*& OutMaterial);

	/** clears the specified parameter, returning it to the default value set in the template
	 * @param ParameterName name of parameter to remove
	 * @param ParameterType type of parameter to remove; if omitted or PSPT_None is specified, all parameters with the given name are removed
	 */
	void ClearParameter(FName ParameterName, enum EParticleSysParamType ParameterType = EParticleSysParamType(0));

	/** Change the ParticleSystem used by this ParticleSystemComponent */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	void SetTemplate(class UParticleSystem* NewTemplate);

	/** Get the current number of active particles in this system */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	int32 GetNumActiveParticles() const;

	/**
	* Fills the passed array with all trail emitters associated with a particular object.
	* @param OutTrailEmitters	The array to fill with pointers to the trail emitters.
	* @param InOwner			The object that triggered this trail. Can be NULL if no assosiation was set by the owner. Not to be confused with the result of GetOwner().
	* @param bSetOwner			If true, all trail emitters will be set as owned by InOwner.
	*/
	virtual void GetOwnedTrailEmitters(TArray< struct FParticleAnimTrailEmitterInstance* >& OutTrailEmitters, const void* InOwner, bool bSetOwner = false);
	
	/**
	* Begins all trail emitters in this component.
	*
	* @param	InFirstSocketName	The name of the first socket for the trail.
	* @param	InSecondSocketName	The name of the second socket for the trail.
	* @param	InWidthMode			How the width value is applied to the trail.
	* @param	InWidth				The width of the trail.
	*/
	UFUNCTION(BlueprintCallable, Category = "Effects|Particles|Trails")
	void BeginTrails(FName InFirstSocketName, FName InSecondSocketName, ETrailWidthMode InWidthMode, float InWidth);

	/**
	* Ends all trail emitters in this component.
	*/
	UFUNCTION(BlueprintCallable, Category = "Effects|Particles|Trails")
	void EndTrails();

	/**
	* Sets the defining data for all trails in this component.
	*
	* @param	InFirstSocketName	The name of the first socket for the trail.
	* @param	InSecondSocketName	The name of the second socket for the trail.
	* @param	InWidthMode			How the width value is applied to the trail.
	* @param	InWidth				The width of the trail.
	*/
	UFUNCTION(BlueprintCallable, Category = "Effects|Particles|Trails")
	void SetTrailSourceData(FName InFirstSocketName, FName InSecondSocketName, ETrailWidthMode InWidthMode, float InWidth);

public:
	TArray<struct FParticleEmitterInstance*> EmitterInstances;

	class FFXSystemInterface* FXSystem;

	/** Command fence used to shut down properly */
	class FRenderCommandFence* ReleaseResourcesFence;

private:
	/** Task ref for parallel portion */
	FGraphEventRef AsyncWork;
	/** Copy of delta time coming into TickComponent */
	float DeltaTimeTick;
	/** Info from async tick */
	int32 TotalActiveParticles;
	/** If true, it means the ASync work is done and the finalize is not */
	bool bNeedsFinalize;
public:

	// Begin UActorComponent interface.
#if WITH_EDITOR
	virtual void CheckForErrors() override;
#endif
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	virtual UObject const* AdditionalStatObject() const override
	{
		return Template;
	}
protected:
	virtual void CreateRenderState_Concurrent() override;
	virtual void SendRenderTransform_Concurrent() override;
	virtual void DestroyRenderState_Concurrent() override;
	virtual void OnRegister() override;
	virtual void OnUnregister()  override;
	virtual void SendRenderDynamicData_Concurrent() override;
public:
	// End UActorComponent interface.

	enum EForceAsyncWorkCompletion
	{
		STALL,
		ENSURE_AND_STALL,
		SILENT, // this would only be appropriate for editor only or other unusual things that we never see in game
	};
	/** If there is async work outstanding, force it to be completed now **/
	FORCEINLINE void ForceAsyncWorkCompletion(EForceAsyncWorkCompletion Behavior) const
	{
		if (AsyncWork.GetReference())
		{
			WaitForAsyncAndFinalize(Behavior);
		}
	}

	/** 
	  * Decide which detail mode should be applied to this particle system. If we have an editor
	  * override specified, use that. Otherwise use the global cached value 
	  */
	int32 GetCurrentDetailMode() const;

private:
	/** Possibly parallel phase of TickComponent **/
	void ComputeTickComponent_Concurrent();

	/** After the possibly parallel phase of TickComponent, we fire events, etc **/
	void FinalizeTickComponent();
	/** Wait on the async task and call finalize on the tick **/
	void WaitForAsyncAndFinalize(EForceAsyncWorkCompletion Behavior) const;

	/** Cache view relevance flags. */
	void CacheViewRelevanceFlags(class UParticleSystem* TemplateToCache);

public:

	// Begin UObject interface.
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
	virtual void FinishDestroy() override;
#if WITH_EDITOR
	virtual void PreEditChange(UProperty* PropertyThatWillChange) override;
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent) override;
#endif // WITH_EDITOR
	virtual void Serialize(FArchive& Ar) override;
	virtual SIZE_T GetResourceSize(EResourceSizeMode::Type Mode) override;
	virtual FString GetDetailedInfoInternal() const override;
	// End UObject interface.

	//Begin UPrimitiveComponent Interface
	virtual int32 GetNumMaterials() const override; 
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* Material) override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual void GetUsedMaterials( TArray<UMaterialInterface*>& OutMaterials ) const override;
	//End UPrimitiveComponent Interface

	// Begin USceneComonent Interface
protected:
	virtual bool ShouldActivate() const override;

public:
	virtual void Activate(bool bReset=false) override;
	virtual void Deactivate() override;
	virtual void ApplyWorldOffset(const FVector& InOffset, bool bWorldShift) override;
	// End USceneComponent Interface

	/** Activate the system */
	void ActivateSystem(bool bFlagAsJustAttached = false);
	/** Deactivate the system */
	void DeactivateSystem();
	// Collision Handling...
	virtual bool ParticleLineCheck(FHitResult& Hit, AActor* SourceActor, const FVector& End, const FVector& Start, const FVector& HalfExtent, const FCollisionObjectQueryParams& ObjectParams);

	/** return true if this psys can tick in any thread */
	FORCEINLINE bool CanTickInAnyThread()
	{
		if (!bIsElligibleForAsyncTickComputed)
		{
			ComputeCanTickInAnyThread();
		}
		return bIsElligibleForAsyncTick;
	}
	/** Decide if this psys can tick in any thread, and set bIsElligibleForAsyncTick */
	void ComputeCanTickInAnyThread();

	/**
	* Creates a Dynamic Material Instance for the specified named material override, optionally from the supplied material.
	* @param Name - The slot name of the material to replace.  If invalid, the material is unchanged and NULL is returned.
	*/
	UFUNCTION(BlueprintCallable, Category = "Rendering|Material")
	virtual class UMaterialInstanceDynamic* CreateNamedDynamicMaterialInstance(FName Name, class UMaterialInterface* SourceMaterial = NULL);

	/** Returns a named material. If this named material is not found, returns NULL. */
	UFUNCTION(BlueprintCallable, Category = "Rendering|Material")
	virtual class UMaterialInterface* GetNamedMaterial(FName Name) const;

	/** Returns the index into the EmitterMaterials array for this named. If there are no named material slots or this material is not found, INDEX_NONE is returned. */
	virtual int32 GetNamedMaterialIndex(FName Name) const;

protected:

	// @todo document
	virtual void UpdateLODInformation();

	/**
	 * Static: Supplied with a chunk of replay data, this method will create dynamic emitter data that can
	 * be used to render the particle system
	 *
	 * @param	EmitterInstance		Emitter instance this replay is playing on
	 * @param	EmitterReplayData	Incoming replay data of any time, cannot be NULL
	 * @param	bSelected			true if the particle system is currently selected
	 *
	 * @return	The newly created dynamic data, or NULL on failure
	 */
	static FDynamicEmitterDataBase* CreateDynamicDataFromReplay( FParticleEmitterInstance* EmitterInstance, const FDynamicEmitterReplayDataBase* EmitterReplayData, bool bSelected );

	/**
	 * Creates dynamic particle data for rendering the particle system this frame.  This function
	 * handle creation of dynamic data for regularly simulated particles, but also handles capture
	 * and playback of particle replay data.
	 *
	 * @return	Returns the dynamic data to render this frame
	 */
	FParticleDynamicData* CreateDynamicData();

	/** Orients the Z axis of the ParticleSystemComponent toward the camera while preserving the X axis direction */
	void OrientZAxisTowardCamera();

	/** Clears dynamic data on the rendering thread. */
	void ClearDynamicData();

	// @todo document
	virtual void UpdateDynamicData(FParticleSystemSceneProxy* Proxy);

public:
	FORCEINLINE int32 GetCurrentLODIndex() const
	{
		return LODLevel;
	}

	// If particles have not already been initialised (ie. EmitterInstances created) do it now.
	virtual void InitParticles();


	// @todo document
	void ResetParticles(bool bEmptyInstances = false);


	// @todo document
	void ResetBurstLists();


	// @todo document
	void UpdateInstances(bool bEmptyInstances = false);


	// @todo document
	bool HasCompleted();


	// @todo document
	void InitializeSystem();

	/**
	 *	Cache the view-relevance for each emitter at each LOD level if needed.
	 *
	 *	@param	NewTemplate		The UParticleSystem* to use as the template.
	 *							If NULL, use the currently set template.
	 */
	void ConditionalCacheViewRelevanceFlags(class UParticleSystem* NewTemplate = NULL);

	/** Auto-populate the instance parameters based on contained modules. */
	void	AutoPopulateInstanceProperties();

	/** Event reporting... */
	/**
	 *	Record a spawning event.
	 *
	 *	@param	InEventName			The name of the event that fired.
	 *	@param	InEmitterTime		The emitter time when the event fired.
	 *	@param	InLocation			The location of the particle when the event fired.
	 *	@param	InVelocity			The velocity of the particle when the event fired.
	 *  @param  InEventData         Gamespecific event data payload
	 */
	void ReportEventSpawn(const FName InEventName, const float InEmitterTime,
		const FVector InLocation, const FVector InVelocity, const TArray<class UParticleModuleEventSendToGame*>& InEventData);

	/**
	 *	Record a death event.
	 *
	 *	@param	InEventName			The name of the event that fired.
	 *	@param	InEmitterTime		The emitter time when the event fired.
	 *	@param	InLocation			The location of the particle when the event fired.
	 *	@param	InVelocity			The velocity of the particle when the event fired.
	 *  @param  InEventData         Gamespecific event data payload
	 *	@param	InParticleTime		The relative life of the particle when the event fired.
	 */
	void ReportEventDeath(const FName InEventName, const float InEmitterTime,
		const FVector InLocation, const FVector InVelocity, const TArray<class UParticleModuleEventSendToGame*>& InEventData, const float InParticleTime);

	/**
	 *	Record a collision event.
	 *
	 *	@param	InEventName		The name of the event that fired.
	 *	@param	InEmitterTime	The emitter time when the event fired.
	 *	@param	InLocation		The location of the particle when the event fired.
	 *	@param	InDirection		The direction of the particle when the event fired.
	 *	@param	InVelocity		The velocity of the particle when the event fired.
	 *  @param  InEventData         Gamespecific event data payload
	 *	@param	InParticleTime	The relative life of the particle when the event fired.
	 *	@param	InNormal		Normal vector of the collision in coordinate system of the returner. Zero=none.
	 *	@param	InTime			Time until hit, if line check.
	 *	@param	InItem			Primitive data item which was hit, INDEX_NONE=none.
	 *	@param	InBoneName		Name of bone we hit (for skeletal meshes).
	 */
	void ReportEventCollision(const FName InEventName, const float InEmitterTime, const FVector InLocation,
		const FVector InDirection, const FVector InVelocity, const TArray<class UParticleModuleEventSendToGame*>& InEventData, 
		const float InParticleTime, const FVector InNormal, const float InTime, const int32 InItem, const FName InBoneName);

	/**
	 *	Record a bursting event.
	 *
	 *	@param	InEventName			The name of the event that fired.
	 *	@param	InEmitterTime		The emitter time when the event fired.
	 *  @param  InParticleCount     The number of particles spawned in the burst
	 *	@param	InLocation			The location of the particle emitter when the event fired.
	 */
	void ReportEventBurst(const FName InEventName, const float InEmitterTime, const int32 ParticleCount,
		const FVector InLocation, const TArray<class UParticleModuleEventSendToGame*>& InEventData);

	/**
	 *	Record a kismet event.
	 *
	 *	@param	InEventName				The name of the event that fired.
	 *	@param	InEmitterTime			The emitter time when the event fired.
	 *	@param	InLocation				The location of the particle when the event fired.
	 *	@param	InVelocity				The velocity of the particle when the event fired.
	 *	@param	InNormal				Normal vector of the collision in coordinate system of the returner. Zero=none.
	 */
	UFUNCTION(BlueprintCallable, Category="Effects|Components|ParticleSystem")
	void GenerateParticleEvent(const FName InEventName, const float InEmitterTime,
		const FVector InLocation, const FVector InDirection, const FVector InVelocity);


	/**
	 * Finds the replay clip of the specified ID number
	 *
	 * @return Returns the replay clip or NULL if none
	 */
	UParticleSystemReplay* FindReplayClipForIDNumber( const int32 InClipIDNumber );

	// @todo document
	void KillParticlesForced();
	
	/** 
	 *	Set a named random vector instance parameter on this ParticleSystemComponent. 
	 *	Updates the parameter if it already exists, or creates a new entry if not. 
	 */
	void SetVectorRandParameter(FName ParameterName, const FVector& Param, const FVector& ParamLow);

	/** 
	 *	Set a named random float instance parameter on this ParticleSystemComponent. 
	 *	Updates the parameter if it already exists, or creates a new entry if not. 
	 */
	void SetFloatRandParameter(FName ParameterName, float Param, float ParamLow);

	/**
	 * Force the component to update its bounding box.
	 */
	void ForceUpdateBounds();	
	

	// @todo document
	virtual void RewindEmitterInstances();
	
	/**
	 * This will determine which LOD to use based off the specific ParticleSystem passed in
	 * and the distance to where that PS is being displayed.
	 *
	 * @note:  This is distance based LOD not perf based.  Perf and distance are orthogonal concepts.
	 */
	virtual int32 DetermineLODLevelForLocation(const FVector& EffectLocation);
	
	/** Set the LOD level of the particle system */
	virtual void SetLODLevel(int32 InLODLevel);
	
	/** Get the LOD level of the particle system */
	virtual int32 GetLODLevel();
	
	/** stops the emitter, unregisters the component, and resets the component's properties to the values of its template */
	void ResetToDefaults();

private:
	/** 
	 * Returns true if this component can and should compute it's LOD without
	 * visibility information from the renderer.
	 */
	bool ShouldComputeLODFromGameThread();
};



