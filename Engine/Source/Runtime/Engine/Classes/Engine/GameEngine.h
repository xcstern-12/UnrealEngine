// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

//=============================================================================
// GameEngine: The game subsystem.
//=============================================================================

#pragma once

#include "Engine/Engine.h"

#include "GameEngine.generated.h"

UCLASS(config=Engine, transient)
class ENGINE_API UGameEngine : public UEngine
{
	GENERATED_UCLASS_BODY()

	/** Check to see if we need to start a movie capture (used on the first tick when we want to record a Matinee) */
	UPROPERTY(transient)
	uint32 bCheckForMovieCapture:1;

	/** Maximium delta time the engine uses to populate FApp::DeltaTime. If 0, unbound. */
	UPROPERTY(config)
	float MaxDeltaTime;

	UPROPERTY(transient)
	UGameInstance* GameInstance;

public:
	/** The game viewport window */
	TWeakPtr<class SWindow> GameViewportWindow;
	/** The primary scene viewport */
	TSharedPtr<class FSceneViewport> SceneViewport;
	/** The game viewport widget */
	TSharedPtr<class SViewport> GameViewportWidget;
	
	/**
	 * Creates the viewport widget where the games Slate UI is added to.
	 */
	void CreateGameViewportWidget( UGameViewportClient* GameViewportClient );

	/**
	 * Creates the game viewport
	 *
	 * @param GameViewportClient	The viewport client to use in the game
	 */
	void CreateGameViewport( UGameViewportClient* GameViewportClient );
	
	/**
	 * Creates the game window
	 */
	static TSharedRef<SWindow> CreateGameWindow();

	/**
	 * Modifies the game window resolution settings if any overrides have been specified on the command line
	 *
	 * @param ResolutionX	[in/out] Width of the game window, in pixels
	 * @param ResolutionY	[in/out] Height of the game window, in pixels
	 * @param WindowMode	[in/out] What window mode the game should be in
	 */
	static void ConditionallyOverrideSettings( int32& ResolutionX, int32& ResolutionY, EWindowMode::Type& WindowMode );
	
	/**
	 * Changes the game window to use the game viewport instead of any loading screen
	 * or movie that might be using it instead
	 */
	void SwitchGameWindowToUseGameViewport();

	/**
	 * Called when the game window closes (ends the game)
	 */
	void OnGameWindowClosed( const TSharedRef<SWindow>& WindowBeingClosed );
	
	/**
	 * Called when the game window is moved
	 */
	void OnGameWindowMoved( const TSharedRef<SWindow>& WindowBeingMoved );

	/**
	 * Redraws all viewports.
	 * @param	bShouldPresent	Whether we want this frame to be presented
	 */
	virtual void RedrawViewports( bool bShouldPresent = true );

	// Begin UObject interface.
	virtual void FinishDestroy() override;
	// End UObject interface.

	// Begin UEngine interface.
	virtual void Init(class IEngineLoop* InEngineLoop) override;
	virtual void PreExit() override;
	virtual void Tick( float DeltaSeconds, bool bIdleMode ) override;
	virtual float GetMaxTickRate( float DeltaTime, bool bAllowFrameRateSmoothing = true ) override;
	virtual void ProcessToggleFreezeCommand( UWorld* InWorld ) override;
	virtual void ProcessToggleFreezeStreamingCommand( UWorld* InWorld ) override;
	// End UEngine interface.

	// Begin FExec Interface
	virtual bool Exec( UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar=*GLog ) override;
	// End FExec Interface

	/** 
	 * Exec command handlers
	 */

	bool HandleReattachComponentsCommand( const TCHAR* Cmd, FOutputDevice& Ar );
	bool HandleCommand( const TCHAR* Cmd, FOutputDevice& Ar );
	bool HandleExitCommand( const TCHAR* Cmd, FOutputDevice& Ar );
	bool HandleGetMaxTickRateCommand( const TCHAR* Cmd, FOutputDevice& Ar );
	bool HandleCancelCommand( const TCHAR* Cmd, FOutputDevice& Ar, UWorld* InWorld );
#if !UE_BUILD_SHIPPING
	bool HandleApplyUserSettingsCommand( const TCHAR* Cmd, FOutputDevice& Ar );
#endif // !UE_BUILD_SHIPPING

	/**
	 * Called from the first Tick after LoadMap() has been called.
	 * Turns off the loading movie if it was started by LoadMap().
	 */
	virtual void PostLoadMap();

	/** Returns the GameViewport widget */
	virtual TSharedPtr<SViewport> GetGameViewportWidget() const
	{
		return GameViewportWidget;
	}

	/** Loads engine startup modules that the game needs to run.  This will also be called when the editor starts up. */
	static void LoadRuntimeEngineStartupModules();


	/**
	 * This is a global, parameterless function used by the online subsystem modules.
	 * It should never be used in gamecode - instead use the appropriate world context function 
	 * in order to properly support multiple concurrent UWorlds.
	 */
	UWorld* GetGameWorld();
};

