// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "PacketHandler.h"
#include "PacketAudit.h"
#include "EncryptionComponent.h"

#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "HAL/ConsoleManager.h"

#include "DDoSDetection.h"
#include "HandlerComponentFactory.h"
#include "ReliabilityHandlerComponent.h"
#include "PacketHandlerProfileConfig.h"

// @todo #JohnB: There is quite a lot of inefficient copying of packet data going on.
//					Redo the whole packet parsing/modification pipeline.

IMPLEMENT_MODULE(FPacketHandlerComponentModuleInterface, PacketHandler);

DEFINE_LOG_CATEGORY(PacketHandlerLog);

DECLARE_CYCLE_STAT(TEXT("PacketHandler Incoming_Internal"), Stat_PacketHandler_Incoming_Internal, STATGROUP_Net);
DECLARE_CYCLE_STAT(TEXT("PacketHandler Outgoing_Internal"), Stat_PacketHandler_Outgoing_Internal, STATGROUP_Net);

/**
 * PacketHandler
 */

PacketHandler::PacketHandler(FDDoSDetection* InDDoS/*=nullptr*/)
	: Mode(Handler::Mode::Client)
	, bConnectionlessHandler(false)
	, DDoS(InDDoS)
	, LowLevelSendDel()
	, LowLevelSendDel_Deprecated()
	, HandshakeCompleteDel()
	, OutgoingPacket()
	, IncomingPacket()
	, HandlerComponents()
	, MaxPacketBits(0)
	, State(Handler::State::Uninitialized)
	, BufferedPackets()
	, QueuedPackets()
	, QueuedRawPackets()
	, QueuedHandlerPackets()
	, BufferedConnectionlessPackets()
	, QueuedConnectionlessPackets()
	, ReliabilityComponent(nullptr)
	, bRawSend(false)
	, Provider()
	, Aggregator()
	, bBeganHandshaking(false)
{
	OutgoingPacket.SetAllowResize(true);
	OutgoingPacket.AllowAppend(true);
}

void PacketHandler::Tick(float DeltaTime)
{
	for (const TSharedPtr<HandlerComponent>& Component : HandlerComponents)
	{
		if (Component.IsValid())
		{
			Component->Tick(DeltaTime);
		}
	}

	// Send off any queued handler packets
	BufferedPacket* QueuedPacket = nullptr;

	while (QueuedHandlerPackets.Dequeue(QueuedPacket))
	{
		check(QueuedPacket->FromComponent != nullptr);

		FBitWriter OutPacket;

		OutPacket.SerializeBits(QueuedPacket->Data, QueuedPacket->CountBits);

		SendHandlerPacket(QueuedPacket->FromComponent, OutPacket, QueuedPacket->Traits);
	}
}

void PacketHandler::Initialize(Handler::Mode InMode, uint32 InMaxPacketBits, bool bConnectionlessOnly/*=false*/,
								TSharedPtr<IAnalyticsProvider> InProvider/*=nullptr*/, FDDoSDetection* InDDoS/*=nullptr*/, FName InDriverProfile/*=NAME_None*/)
{
	Mode = InMode;
	MaxPacketBits = InMaxPacketBits;
	DDoS = InDDoS;

	// @todo #JohnB: Redo this, so you don't load from the .ini at all, have it hardcoded elsewhere - do not want this in shipping.

	bConnectionlessHandler = bConnectionlessOnly;

	// Only UNetConnection's will load the .ini components, for now.
	if (!bConnectionlessHandler)
	{
		TArray<FString> Components;
		UPacketHandlerProfileConfig* CurNetDriverProfile = NewObject<UPacketHandlerProfileConfig>((UObject*)GetTransientPackage(), InDriverProfile);
		Components.Append(CurNetDriverProfile->Components);
		
		// If we didn't get any matches, push in the regular components.
		if (Components.Num() == 0)
		{
			GConfig->GetArray(TEXT("PacketHandlerComponents"), TEXT("Components"), Components, GEngineIni);
		}

		for (const FString& CurComponent : Components)
		{
			AddHandler(CurComponent, true);
		}
	}

	// Add encryption component, if configured.
	FString EncryptionComponentName;
	if (GConfig->GetString(TEXT("PacketHandlerComponents"), TEXT("EncryptionComponent"), EncryptionComponentName, GEngineIni) && !EncryptionComponentName.IsEmpty())
	{
		static IConsoleVariable* const AllowEncryptionCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.AllowEncryption"));
		if (AllowEncryptionCVar == nullptr || AllowEncryptionCVar->GetInt() != 0)
		{
			EncryptionComponent = StaticCastSharedPtr<FEncryptionComponent>(AddHandler(EncryptionComponentName, true));
		}
		else
		{
			UE_LOG(PacketHandlerLog, Warning, TEXT("PacketHandler encryption component is configured as %s, but it won't be used because the cvar net.AllowEncryption is false."), *EncryptionComponentName);
		}
	}

	bool bEnableReliability = false;

	GConfig->GetBool(TEXT("PacketHandlerComponents"), TEXT("bEnableReliability"), bEnableReliability, GEngineIni);

	if (bEnableReliability && !ReliabilityComponent.IsValid())
	{
		TSharedPtr<HandlerComponent> NewComponent = MakeShareable(new ReliabilityHandlerComponent);
		ReliabilityComponent = StaticCastSharedPtr<ReliabilityHandlerComponent>(NewComponent);
		AddHandler(NewComponent, true);
	}
}

void PacketHandler::NotifyAnalyticsProvider(TSharedPtr<IAnalyticsProvider> InProvider, TSharedPtr<FNetAnalyticsAggregator> InAggregator)
{
	Provider = InProvider;
	Aggregator = InAggregator;

	if (State != Handler::State::Uninitialized)
	{
		// Hotfixes should never reach this code from NetConnection's, but we can't avoid it in the case of the stateless connect handler.
		// The latter should be ok without special/expensive multithreaded handling, as the hotfix happens so early - but it's not ideal.
		ensure(bConnectionlessHandler || HandlerComponents.Num() == 0);

		for (const TSharedPtr<HandlerComponent>& CurComponent : HandlerComponents)
		{
			if (CurComponent->IsInitialized())
			{
				CurComponent->NotifyAnalyticsProvider();
			}
		}
	}
}

void PacketHandler::InitializeComponents()
{
	if (State == Handler::State::Uninitialized)
	{
		if (HandlerComponents.Num() > 0)
		{
			SetState(Handler::State::InitializingComponents);
		}
		else
		{
			HandlerInitialized();
		}
	}

	// Trigger delayed-initialization for HandlerComponents
	for (TSharedPtr<HandlerComponent>& Component : HandlerComponents)
	{
		if (Component.IsValid() && !Component->IsInitialized())
		{
			Component->Initialize();
			Component->NotifyAnalyticsProvider();
		}
	}

	// Called early, to ensure that all handlers report a valid reserved packet bits value (triggers an assert if not)
	GetTotalReservedPacketBits();
}

void PacketHandler::BeginHandshaking(FPacketHandlerHandshakeComplete InHandshakeDel/*=FPacketHandlerHandshakeComplete()*/)
{
	check(!bBeganHandshaking);

	bBeganHandshaking = true;

	HandshakeCompleteDel = InHandshakeDel;

	for (int32 i=HandlerComponents.Num() - 1; i>=0; --i)
	{
		HandlerComponent& CurComponent = *HandlerComponents[i];

		if (CurComponent.RequiresHandshake() && !CurComponent.IsInitialized())
		{
			CurComponent.NotifyHandshakeBegin();
			break;
		}
	}
}

void PacketHandler::AddHandler(TSharedPtr<HandlerComponent>& NewHandler, bool bDeferInitialize/*=false*/)
{
	// This is never valid. Can end up silently changing maximum allow packet size, which could cause failure to send packets.
	if (State != Handler::State::Uninitialized)
	{
		LowLevelFatalError(TEXT("Handler added during runtime."));
		return;
	}

	// This should always be fatal, as an unexpectedly missing handler, may break net compatibility with the remote server/client
	if (!NewHandler.IsValid())
	{
		LowLevelFatalError(TEXT("Failed to add handler - invalid instance."));
		return;
	}

	// Warn if a component already exists with the same name.
	const bool bNameAlreadyExists = HandlerComponents.ContainsByPredicate([NewHandler](const TSharedPtr<HandlerComponent>& Component)
	{
		return Component->GetName() == NewHandler->GetName();
	});

	if (bNameAlreadyExists)
	{
		UE_LOG(PacketHandlerLog, Warning, TEXT("Packet handler already contains a component with name %s."), *NewHandler->GetName().ToString());
		return;
	}

	HandlerComponents.Add(NewHandler);
	NewHandler->Handler = this;

	if (!bDeferInitialize)
	{
		NewHandler->Initialize();
	}
}

TSharedPtr<HandlerComponent> PacketHandler::AddHandler(const FString& ComponentStr, bool bDeferInitialize/*=false*/)
{
	TSharedPtr<HandlerComponent> ReturnVal = nullptr;

	if (!ComponentStr.IsEmpty())
	{
		FString ComponentName;
		FString ComponentOptions;

		for (int32 i=0; i<ComponentStr.Len(); i++)
		{
			TCHAR c = ComponentStr[i];

			// Parsing Options
			if (c == '(')
			{
				// Skip '('
				++i;

				// Parse until end of options
				for (; i<ComponentStr.Len(); i++)
				{
					c = ComponentStr[i];

					// End of options
					if (c == ')')
					{
						break;
					}
					// Append char to options
					else
					{
						ComponentOptions.AppendChar(c);
					}
				}
			}
			// Append char to component name if not whitespace
			else if (c != ' ')
			{
				ComponentName.AppendChar(c);
			}
		}

		if (ComponentName != TEXT("ReliabilityHandlerComponent"))
		{
			int32 FactoryComponentDelim = ComponentName.Find(TEXT("."));

			if (FactoryComponentDelim != INDEX_NONE)
			{
				// Every HandlerComponentFactory type has one instance, loaded as a named singleton
				FString SingletonName = ComponentName.Mid(FactoryComponentDelim + 1) + TEXT("_Singleton");
				UHandlerComponentFactory* Factory = FindObject<UHandlerComponentFactory>(ANY_PACKAGE, *SingletonName);

				if (Factory == nullptr)
				{
					UClass* FactoryClass = StaticLoadClass(UHandlerComponentFactory::StaticClass(), nullptr, *ComponentName);

					if (FactoryClass != nullptr)
					{
						Factory = NewObject<UHandlerComponentFactory>(GetTransientPackage(), FactoryClass, *SingletonName);
					}
				}


				if (Factory != nullptr)
				{
					ReturnVal = Factory->CreateComponentInstance(ComponentOptions);
				}
				else
				{
					UE_LOG(PacketHandlerLog, Warning, TEXT("Unable to load HandlerComponent factory: %s"), *ComponentName);
				}
			}
			// @todo #JohnB: Deprecate non-factory components eventually
			else
			{
				FPacketHandlerComponentModuleInterface* PacketHandlerInterface = FModuleManager::Get().LoadModulePtr<FPacketHandlerComponentModuleInterface>(FName(*ComponentName));

				if (PacketHandlerInterface)
				{
					ReturnVal = PacketHandlerInterface->CreateComponentInstance(ComponentOptions);
				}
				else
				{
					UE_LOG(PacketHandlerLog, Warning, TEXT("Unable to Load Module: %s"), *ComponentName);
				}
			}


			if (ReturnVal.IsValid())
			{
				UE_LOG(PacketHandlerLog, Log, TEXT("Loaded PacketHandler component: %s (%s)"), *ComponentName,
						*ComponentOptions);

				AddHandler(ReturnVal, bDeferInitialize);
			}
		}
		else
		{
			UE_LOG(PacketHandlerLog, Warning, TEXT("PacketHandlerComponent 'ReliabilityHandlerComponent' is internal-only."));
		}
	}

	return ReturnVal;
}

void PacketHandler::IncomingHigh(FBitReader& Reader)
{
	// @todo #JohnB
}

void PacketHandler::OutgoingHigh(FBitWriter& Writer)
{
	// @todo #JohnB
}

TSharedPtr<FEncryptionComponent> PacketHandler::GetEncryptionComponent()
{
	return EncryptionComponent;
}

TSharedPtr<HandlerComponent> PacketHandler::GetComponentByName(FName ComponentName) const
{
	for (const TSharedPtr<HandlerComponent>& Component : HandlerComponents)
	{
		if (Component.IsValid() && Component->GetName() == ComponentName)
		{
			return Component;
		}
	}

	return nullptr;
}

const ProcessedPacket PacketHandler::Incoming_Internal(uint8* Packet, int32 CountBytes, bool bConnectionless, const FString& Address)
{
	SCOPE_CYCLE_COUNTER(Stat_PacketHandler_Incoming_Internal);

	// @todo #JohnB: Try to optimize this function more, seeing as it will be a common codepath DoS attacks pass through
	// @todo #JohnB: Clean up returns.

	int32 CountBits = CountBytes * 8;
	bool bError = false;

	if (HandlerComponents.Num() > 0)
	{
		uint8 LastByte = Packet[CountBytes - 1];

		if (LastByte != 0)
		{
			CountBits--;

			while (!(LastByte & 0x80))
			{
				LastByte *= 2;
				CountBits--;
			}
		}
		else
		{
			bError = true;

#if !UE_BUILD_SHIPPING
			UE_CLOG((DDoS == nullptr || !DDoS->CheckLogRestrictions()), PacketHandlerLog, Error,
					TEXT("PacketHandler parsing packet with zero's in last byte."));
#endif
		}
	}


	if (!bError)
	{
		FBitReader ProcessedPacketReader(Packet, CountBits);

		FPacketAudit::CheckStage(TEXT("PostPacketHandler"), ProcessedPacketReader);

		if (State == Handler::State::Uninitialized)
		{
			UpdateInitialState();
		}


		for (int32 i=HandlerComponents.Num() - 1; i>=0; --i)
		{
			HandlerComponent& CurComponent = *HandlerComponents[i];

			if (CurComponent.IsActive() && !ProcessedPacketReader.IsError() && ProcessedPacketReader.GetBitsLeft() > 0)
			{
				// Realign the packet, so the packet data starts at position 0, if necessary
				if (ProcessedPacketReader.GetPosBits() != 0 && !CurComponent.CanReadUnaligned())
				{
					RealignPacket(ProcessedPacketReader);
				}

				if (bConnectionless)
				{
					CurComponent.IncomingConnectionless(Address, ProcessedPacketReader);
				}
				else
				{
					CurComponent.Incoming(ProcessedPacketReader);
				}
			}
		}

		if (!ProcessedPacketReader.IsError())
		{
			ReplaceIncomingPacket(ProcessedPacketReader);

			if (IncomingPacket.GetBitsLeft() > 0)
			{
				FPacketAudit::CheckStage(TEXT("PrePacketHandler"), IncomingPacket, true);
			}

			return ProcessedPacket(IncomingPacket.GetData(), IncomingPacket.GetBitsLeft());
		}
		else
		{
			return ProcessedPacket(nullptr, 0, true);
		}
	}
	else
	{
		return ProcessedPacket(nullptr, 0, true);
	}
}

const ProcessedPacket PacketHandler::Outgoing_Internal(uint8* Packet, int32 CountBits, FOutPacketTraits& Traits, bool bConnectionless, const FString& Address)
{
	SCOPE_CYCLE_COUNTER(Stat_PacketHandler_Outgoing_Internal);

	if (!bRawSend)
	{
		OutgoingPacket.Reset();

		if (State == Handler::State::Uninitialized)
		{
			UpdateInitialState();
		}


		if (State == Handler::State::Initialized)
		{
			OutgoingPacket.SerializeBits(Packet, CountBits);

			FPacketAudit::AddStage(TEXT("PrePacketHandler"), OutgoingPacket, true);

			for (int32 i=0; i<HandlerComponents.Num() && !OutgoingPacket.IsError(); ++i)
			{
				HandlerComponent& CurComponent = *HandlerComponents[i];

				if (CurComponent.IsActive())
				{
					if (OutgoingPacket.GetNumBits() <= CurComponent.MaxOutgoingBits)
					{
						if (bConnectionless)
						{
							CurComponent.OutgoingConnectionless(Address, OutgoingPacket, Traits);
						}
						else
						{
							CurComponent.Outgoing(OutgoingPacket, Traits);
						}
					}
					else
					{
						OutgoingPacket.SetError();

						UE_LOG(PacketHandlerLog, Error, TEXT("Packet exceeded HandlerComponents 'MaxOutgoingBits' value: %i vs %i"),
								OutgoingPacket.GetNumBits(), CurComponent.MaxOutgoingBits);

						break;
					}
				}
			}

			// Add a termination bit, the same as the UNetConnection code does, if appropriate
			if (HandlerComponents.Num() > 0 && OutgoingPacket.GetNumBits() > 0)
			{
				FPacketAudit::AddStage(TEXT("PostPacketHandler"), OutgoingPacket);

				OutgoingPacket.WriteBit(1);
			}

			if (!bConnectionless && ReliabilityComponent.IsValid() && OutgoingPacket.GetNumBits() > 0)
			{
				// Let the reliability handler know about all processed packets, so it can record them for resending if needed
				ReliabilityComponent->QueuePacketForResending(OutgoingPacket.GetData(), OutgoingPacket.GetNumBits(), Traits);
			}
		}
		// Buffer any packets being sent from game code until processors are initialized
		else if (State == Handler::State::InitializingComponents && CountBits > 0)
		{
			if (bConnectionless)
			{
				BufferedConnectionlessPackets.Add(new BufferedPacket(Address, Packet, CountBits, Traits));
			}
			else
			{
				BufferedPackets.Add(new BufferedPacket(Packet, CountBits, Traits));
			}

			Packet = nullptr;
			CountBits = 0;
		}

		// @todo #JohnB: Tidy up return code
		if (!OutgoingPacket.IsError())
		{
			return ProcessedPacket(OutgoingPacket.GetData(), OutgoingPacket.GetNumBits());
		}
		else
		{
			return ProcessedPacket(nullptr, 0, true);
		}
	}
	else
	{
		return ProcessedPacket(Packet, CountBits);
	}
}

void PacketHandler::ReplaceIncomingPacket(FBitReader& ReplacementPacket)
{
	if (ReplacementPacket.GetPosBits() == 0 || ReplacementPacket.GetBitsLeft() == 0)
	{
		IncomingPacket = MoveTemp(ReplacementPacket);
	}
	else
	{
		// @todo #JohnB: Make this directly adjust and write into IncomingPacket's buffer, instead of copying - very inefficient
		TArray<uint8> TempPacketData;
		TempPacketData.AddUninitialized(ReplacementPacket.GetBytesLeft());
		TempPacketData[TempPacketData.Num()-1] = 0;

		int64 NewPacketSizeBits = ReplacementPacket.GetBitsLeft();

		ReplacementPacket.SerializeBits(TempPacketData.GetData(), NewPacketSizeBits);
		IncomingPacket.SetData(MoveTemp(TempPacketData), NewPacketSizeBits);
	}
}

void PacketHandler::RealignPacket(FBitReader& Packet)
{
	if (Packet.GetPosBits() != 0)
	{
		uint32 BitsLeft = Packet.GetBitsLeft();

		if (BitsLeft > 0)
		{
			// @todo #JohnB: Based on above - when you optimize above, optimize this too
			TArray<uint8> TempPacketData;
			TempPacketData.AddUninitialized(Packet.GetBytesLeft());
			TempPacketData[TempPacketData.Num()-1] = 0;

			Packet.SerializeBits(TempPacketData.GetData(), BitsLeft);
			Packet.SetData(MoveTemp(TempPacketData), BitsLeft);
		}
	}
}

void PacketHandler::SendHandlerPacket(HandlerComponent* InComponent, FBitWriter& Writer, FOutPacketTraits& Traits)
{
	// @todo #JohnB: There is duplication between this function and others, it would be nice to reduce this.

	// Prevent any cases where a send happens before the handler is ready.
	check(State != Handler::State::Uninitialized);

	if (LowLevelSendDel.IsBound() || LowLevelSendDel_Deprecated.IsBound())
	{
		bool bEncounteredComponent = false;

		for (int32 i=0; i<HandlerComponents.Num() && !Writer.IsError(); ++i)
		{
			HandlerComponent& CurComponent = *HandlerComponents[i];

			// Only process the packet through components coming after the specified one
			if (!bEncounteredComponent)
			{
				if (&CurComponent == InComponent)
				{
					bEncounteredComponent = true;
				}

				continue;
			}

			if (CurComponent.IsActive())
			{
				if (Writer.GetNumBits() <= CurComponent.MaxOutgoingBits)
				{
					CurComponent.Outgoing(Writer, Traits);
				}
				else
				{
					Writer.SetError();

					UE_LOG(PacketHandlerLog, Error, TEXT("Handler packet exceeded HandlerComponents 'MaxOutgoingBits' value: %i vs %i"),
							Writer.GetNumBits(), CurComponent.MaxOutgoingBits);

					break;
				}
			}
		}

		if (!Writer.IsError() && Writer.GetNumBits() > 0)
		{
			FPacketAudit::AddStage(TEXT("PostPacketHandler"), Writer);

			// Add a termination bit, the same as the UNetConnection code does, if appropriate
			Writer.WriteBit(1);


			if (ReliabilityComponent.IsValid())
			{
				// Let the reliability handler know about all processed packets, so it can record them for resending if needed
				ReliabilityComponent->QueueHandlerPacketForResending(InComponent, Writer.GetData(), Writer.GetNumBits(), Traits);
			}

			// Now finish off with a raw send (as we don't want to go through the PacketHandler chain again)
			bool bOldRawSend = bRawSend;

			bRawSend = true;

			LowLevelSendDel_Deprecated.ExecuteIfBound(Writer.GetData(), Writer.GetNumBytes(), Writer.GetNumBits());
			LowLevelSendDel.ExecuteIfBound(Writer.GetData(), Writer.GetNumBits(), Traits);

			bRawSend = bOldRawSend;
		}
	}
	else
	{
		LowLevelFatalError(TEXT("Called SendHandlerPacket when no LowLevelSend delegate is bound"));
	}
}

void PacketHandler::SetState(Handler::State InState)
{
	if (InState == State)
	{
		LowLevelFatalError(TEXT("Set new Packet Processor State to the state it is currently in."));
	} 
	else
	{
		State = InState;
	}
}

void PacketHandler::UpdateInitialState()
{
	if (State == Handler::State::Uninitialized)
	{
		if (HandlerComponents.Num() > 0)
		{
			InitializeComponents();
		} 
		else
		{
			HandlerInitialized();
		}
	}
}

void PacketHandler::HandlerInitialized()
{
	// Quickly verify that, if reliability is required, that it is enabled
	if (!ReliabilityComponent.IsValid())
	{
		for(const TSharedPtr<HandlerComponent>& Component : HandlerComponents)
		{
			if (Component.IsValid() && Component->RequiresReliability())
			{
				// Don't allow this to be missed in shipping - but allow it during development,
				// as this is valid when developing new HandlerComponent's
#if UE_BUILD_SHIPPING
				UE_LOG(PacketHandlerLog, Fatal, TEXT("Some HandlerComponents require bEnableReliability!!!"));
#else
				UE_LOG(PacketHandlerLog, Warning, TEXT("Some HandlerComponents require bEnableReliability!!!"));
#endif

				break;
			}
		}
	}

	// If any buffered packets, add to queue
	for (int32 i=0; i<BufferedPackets.Num(); ++i)
	{
		QueuedPackets.Enqueue(BufferedPackets[i]);
		BufferedPackets[i] = nullptr;
	}

	BufferedPackets.Empty();

	for (int32 i=0; i<BufferedConnectionlessPackets.Num(); ++i)
	{
		QueuedConnectionlessPackets.Enqueue(BufferedConnectionlessPackets[i]);
		BufferedConnectionlessPackets[i] = nullptr;
	}

	BufferedConnectionlessPackets.Empty();

	SetState(Handler::State::Initialized);

	if (bBeganHandshaking)
	{
		HandshakeCompleteDel.ExecuteIfBound();
	}
}

void PacketHandler::HandlerComponentInitialized(HandlerComponent* InComponent)
{
	// Check if all handlers are initialized
	if (State != Handler::State::Initialized)
	{
		bool bAllInitialized = true;
		bool bEncounteredComponent = false;
		bool bPassedHandshakeNotify = false;

		for (int32 i=HandlerComponents.Num() - 1; i>=0; --i)
		{
			HandlerComponent& CurComponent = *HandlerComponents[i];

			if (!CurComponent.IsInitialized())
			{
				bAllInitialized = false;
			}

			if (bEncounteredComponent)
			{
				// If the initialized component required a handshake, pass on notification to the next handshaking component
				// (components closer to the Socket, perform their handshake first)
				if (bBeganHandshaking && !CurComponent.IsInitialized() && InComponent->RequiresHandshake() && !bPassedHandshakeNotify &&
						CurComponent.RequiresHandshake())
				{
					CurComponent.NotifyHandshakeBegin();
					bPassedHandshakeNotify = true;
				}
			}
			else
			{
				bEncounteredComponent = &CurComponent == InComponent;
			}
		}

		if (bAllInitialized)
		{
			HandlerInitialized();
		}
	}
}

bool PacketHandler::DoesAnyProfileHaveComponent(const FString& InComponentName)
{
	TArray<FString> ProfileSectionNames;
	if (GConfig->GetPerObjectConfigSections(GEngineIni, TEXT("PacketHandlerProfileConfig"), ProfileSectionNames))
	{
		for (const FString& CurProfileSection : ProfileSectionNames)
		{
			FName CurNetDriver(*CurProfileSection.Left(CurProfileSection.Find(TEXT(" "))));
			if (DoesProfileHaveComponent(CurNetDriver, InComponentName))
			{
				return true;
			}
		}
	}

	return false;
}

bool PacketHandler::DoesProfileHaveComponent(const FName InNetDriverName, const FString& InComponentName)
{
	TArray<FString> Components;
	// If this call was made during a module load, we cannot use the packethandler config object
	if (GetTransientPackage() == nullptr)
	{
		FString DriverProfileCategory = FString::Printf(TEXT("%s PacketHandlerProfileConfig"), *InNetDriverName.ToString());
		GConfig->GetArray(*DriverProfileCategory, TEXT("Components"), Components, GEngineIni);
	}
	else
	{
		UPacketHandlerProfileConfig* CurNetDriverProfile = NewObject<UPacketHandlerProfileConfig>((UObject*)GetTransientPackage(), InNetDriverName);
		Components.Append(CurNetDriverProfile->Components);
	}
	
	for (const FString& Component : Components)
	{
		if (Component.Contains(InComponentName, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

BufferedPacket* PacketHandler::GetQueuedPacket()
{
	BufferedPacket* QueuedPacket = nullptr;

	QueuedPackets.Dequeue(QueuedPacket);

	return QueuedPacket;
}

BufferedPacket* PacketHandler::GetQueuedRawPacket()
{
	BufferedPacket* QueuedPacket = nullptr;

	QueuedRawPackets.Dequeue(QueuedPacket);

	return QueuedPacket;
}

BufferedPacket* PacketHandler::GetQueuedConnectionlessPacket()
{
	BufferedPacket* QueuedConnectionlessPacket = nullptr;

	QueuedConnectionlessPackets.Dequeue(QueuedConnectionlessPacket);

	return QueuedConnectionlessPacket;
}

int32 PacketHandler::GetTotalReservedPacketBits()
{
	int32 ReturnVal = 0;
	uint32 CurMaxOutgoingBits = MaxPacketBits;

	for (int32 i=HandlerComponents.Num()-1; i>=0; i--)
	{
		HandlerComponent* CurComponent = HandlerComponents[i].Get();
		int32 CurReservedBits = CurComponent->GetReservedPacketBits();

		// Specifying the reserved packet bits is mandatory, even if zero (as accidentally forgetting, leads to hard to trace issues).
		if (CurReservedBits == -1)
		{
			LowLevelFatalError(TEXT("Handler returned invalid 'GetReservedPacketBits' value."));
			continue;
		}


		// Set the maximum Outgoing packet size for the HandlerComponent
		CurComponent->MaxOutgoingBits = CurMaxOutgoingBits;
		CurMaxOutgoingBits -= CurReservedBits;

		ReturnVal += CurReservedBits;
	}


	// Reserve space for the termination bit
	if (HandlerComponents.Num() > 0)
	{
		ReturnVal++;
	}

	return ReturnVal;
}


/**
 * HandlerComponent
 */

HandlerComponent::HandlerComponent()
	: Handler(nullptr)
	, State(Handler::Component::State::UnInitialized)
	, MaxOutgoingBits(0)
	, bRequiresHandshake(false)
	, bRequiresReliability(false)
	, bActive(false)
	, bInitialized(false)
{
}

HandlerComponent::HandlerComponent(FName InName)
	: Handler(nullptr)
	, State(Handler::Component::State::UnInitialized)
	, MaxOutgoingBits(0)
	, bRequiresHandshake(false)
	, bRequiresReliability(false)
	, bActive(false)
	, bInitialized(false)
	, Name(InName)
{
}

bool HandlerComponent::IsActive() const
{
	return bActive;
}

void HandlerComponent::SetActive(bool Active)
{
	bActive = Active;
}

void HandlerComponent::SetState(Handler::Component::State InState)
{
	State = InState;
}

void HandlerComponent::Initialized()
{
	bInitialized = true;
	Handler->HandlerComponentInitialized(this);
}

bool HandlerComponent::IsInitialized() const
{
	return bInitialized;
}

/**
 * FPacketHandlerComponentModuleInterface
 */
void FPacketHandlerComponentModuleInterface::StartupModule()
{
	FPacketAudit::Init();
}

void FPacketHandlerComponentModuleInterface::ShutdownModule()
{
	FPacketAudit::Destruct();
}

