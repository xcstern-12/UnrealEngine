// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	VulkanDevice.cpp: Vulkan device RHI implementation.
=============================================================================*/

#include "VulkanRHIPrivate.h"
#include "VulkanDevice.h"
#include "VulkanPendingState.h"
#include "VulkanContext.h"
#include "Misc/Paths.h"

FVulkanDevice::FVulkanDevice(VkPhysicalDevice InGpu)
	: Gpu(InGpu)
	, Device(VK_NULL_HANDLE)
	, ResourceHeapManager(this)
	, DeferredDeletionQueue(this)
	, DefaultSampler(VK_NULL_HANDLE)
	, TimestampQueryPool(nullptr)
	, GfxQueue(nullptr)
	, TransferQueue(nullptr)
	, ImmediateContext(nullptr)
#if VULKAN_ENABLE_DRAW_MARKERS
	, CmdDbgMarkerBegin(nullptr)
	, CmdDbgMarkerEnd(nullptr)
	, DebugMarkerSetObjectName(nullptr)
#endif
	, PipelineStateCache(nullptr)
{
	FMemory::Memzero(GpuProps);
	FMemory::Memzero(Features);
	FMemory::Memzero(FormatProperties);
	FMemory::Memzero(PixelFormatComponentMapping);
}

FVulkanDevice::~FVulkanDevice()
{
	if (Device != VK_NULL_HANDLE)
	{
		Destroy();
		Device = VK_NULL_HANDLE;
	}
}

void FVulkanDevice::CreateDevice()
{
	check(Device == VK_NULL_HANDLE);

	// Setup extension and layer info
	VkDeviceCreateInfo DeviceInfo;
	FMemory::Memzero(DeviceInfo);
	DeviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

	bool bDebugMarkersFound = false;
	TArray<const ANSICHAR*> DeviceExtensions;
	TArray<const ANSICHAR*> ValidationLayers;
	GetDeviceExtensions(DeviceExtensions, ValidationLayers, bDebugMarkersFound);

	ParseOptionalDeviceExtensions(DeviceExtensions);

	DeviceInfo.enabledExtensionCount = DeviceExtensions.Num();
	DeviceInfo.ppEnabledExtensionNames = DeviceExtensions.GetData();

	DeviceInfo.enabledLayerCount = ValidationLayers.Num();
	DeviceInfo.ppEnabledLayerNames = (DeviceInfo.enabledLayerCount > 0) ? ValidationLayers.GetData() : nullptr;

	// Setup Queue info
	TArray<VkDeviceQueueCreateInfo> QueueFamilyInfos;
	int32 GfxQueueFamilyIndex = -1;
	int32 TransferQueueFamilyIndex = -1;
	UE_LOG(LogVulkanRHI, Display, TEXT("Found %d Queue Families"), QueueFamilyProps.Num());
	uint32 NumPriorities = 0;
	for (int32 FamilyIndex = 0; FamilyIndex < QueueFamilyProps.Num(); ++FamilyIndex)
	{
		const VkQueueFamilyProperties& CurrProps = QueueFamilyProps[FamilyIndex];

		bool bIsValidQueue = false;
		if ((CurrProps.queueFlags & VK_QUEUE_GRAPHICS_BIT) == VK_QUEUE_GRAPHICS_BIT)
		{
			if (GfxQueueFamilyIndex == -1)
			{
				GfxQueueFamilyIndex = FamilyIndex;
				bIsValidQueue = true;
			}
			else
			{
				//#todo-rco: Support for multi-queue/choose the best queue!
			}
		}

		if ((CurrProps.queueFlags & VK_QUEUE_TRANSFER_BIT) == VK_QUEUE_TRANSFER_BIT)
		{
			// Prefer a non-gfx transfer queue
			if (TransferQueueFamilyIndex == -1 && (CurrProps.queueFlags & VK_QUEUE_GRAPHICS_BIT) != VK_QUEUE_GRAPHICS_BIT)
			{
				TransferQueueFamilyIndex = FamilyIndex;
				bIsValidQueue = true;
			}
		}

		auto GetQueueInfoString = [&]()
		{
			FString Info;
			if ((CurrProps.queueFlags & VK_QUEUE_GRAPHICS_BIT) == VK_QUEUE_GRAPHICS_BIT)
			{
				Info += TEXT(" Gfx");
			}
			if ((CurrProps.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT)
			{
				Info += TEXT(" Compute");
			}
			if ((CurrProps.queueFlags & VK_QUEUE_TRANSFER_BIT) == VK_QUEUE_TRANSFER_BIT)
			{
				Info += TEXT(" Xfer");
			}
			if ((CurrProps.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) == VK_QUEUE_SPARSE_BINDING_BIT)
			{
				Info += TEXT(" Sparse");
			}

			return Info;
		};

		if (!bIsValidQueue)
		{
			UE_LOG(LogVulkanRHI, Display, TEXT("Skipping unnecessary Queue Family %d: %d queues%s"), FamilyIndex, CurrProps.queueCount, *GetQueueInfoString());
			continue;
		}

		int32 QueueIndex = QueueFamilyInfos.Num();
		QueueFamilyInfos.AddZeroed(1);
		VkDeviceQueueCreateInfo& CurrQueue = QueueFamilyInfos[QueueIndex];
		CurrQueue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		CurrQueue.queueFamilyIndex = FamilyIndex;
		CurrQueue.queueCount = CurrProps.queueCount;
		NumPriorities += CurrProps.queueCount;
		UE_LOG(LogVulkanRHI, Display, TEXT("Initializing Queue Family %d: %d queues%s"), FamilyIndex, CurrProps.queueCount, *GetQueueInfoString());
	}

	TArray<float> QueuePriorities;
	QueuePriorities.AddUninitialized(NumPriorities);
	float* CurrentPriority = QueuePriorities.GetData();
	for (int32 Index = 0; Index < QueueFamilyInfos.Num(); ++Index)
	{
		VkDeviceQueueCreateInfo& CurrQueue = QueueFamilyInfos[Index];
		CurrQueue.pQueuePriorities = CurrentPriority;

		const VkQueueFamilyProperties& CurrProps = QueueFamilyProps[CurrQueue.queueFamilyIndex];
		for (int32 QueueIndex = 0; QueueIndex < (int32)CurrProps.queueCount; ++QueueIndex)
		{
			*CurrentPriority++ = 1.0f;
		}
	}

	DeviceInfo.queueCreateInfoCount = QueueFamilyInfos.Num();
	DeviceInfo.pQueueCreateInfos = QueueFamilyInfos.GetData();

	DeviceInfo.pEnabledFeatures = &Features;

	// Create the device
	VERIFYVULKANRESULT(VulkanRHI::vkCreateDevice(Gpu, &DeviceInfo, nullptr, &Device));

	// Create Graphics Queue, here we submit command buffers for execution
	GfxQueue = new FVulkanQueue(this, GfxQueueFamilyIndex, 0);
	if (TransferQueueFamilyIndex == -1)
	{
		// If we didn't find a dedicated Queue, use the default one
		TransferQueueFamilyIndex = GfxQueueFamilyIndex;
	}
	TransferQueue = new FVulkanQueue(this, TransferQueueFamilyIndex, 0);

#if VULKAN_ENABLE_DRAW_MARKERS
	if (bDebugMarkersFound || VULKAN_ENABLE_DUMP_LAYER)
	{
		CmdDbgMarkerBegin = (PFN_vkCmdDebugMarkerBeginEXT)(void*)VulkanRHI::vkGetDeviceProcAddr(Device, "vkCmdDebugMarkerBeginEXT");
		CmdDbgMarkerEnd = (PFN_vkCmdDebugMarkerEndEXT)(void*)VulkanRHI::vkGetDeviceProcAddr(Device, "vkCmdDebugMarkerEndEXT");
		DebugMarkerSetObjectName = (PFN_vkDebugMarkerSetObjectNameEXT)(void*)VulkanRHI::vkGetDeviceProcAddr(Device, "vkDebugMarkerSetObjectNameEXT");

		// We're running under RenderDoc or other trace tool, so enable capturing mode
		GDynamicRHI->EnableIdealGPUCaptureOptions(true);
	}
#endif
}

void FVulkanDevice::SetupFormats()
{
	for (uint32 Index = 0; Index < VK_FORMAT_RANGE_SIZE; ++Index)
	{
		const VkFormat Format = (VkFormat)Index;
		FMemory::Memzero(FormatProperties[Index]);
		VulkanRHI::vkGetPhysicalDeviceFormatProperties(Gpu, Format, &FormatProperties[Index]);
	}

	static_assert(sizeof(VkFormat) <= sizeof(GPixelFormats[0].PlatformFormat), "PlatformFormat must be increased!");

	// Initialize the platform pixel format map.
	for (int32 Index = 0; Index < PF_MAX; ++Index)
	{
		GPixelFormats[Index].PlatformFormat = VK_FORMAT_UNDEFINED;
		GPixelFormats[Index].Supported = false;

		// Set default component mapping
		VkComponentMapping& ComponentMapping = PixelFormatComponentMapping[Index];
		ComponentMapping.r = VK_COMPONENT_SWIZZLE_R;
		ComponentMapping.g = VK_COMPONENT_SWIZZLE_G;
		ComponentMapping.b = VK_COMPONENT_SWIZZLE_B;
		ComponentMapping.a = VK_COMPONENT_SWIZZLE_A;
	}

	// Default formats
	MapFormatSupport(PF_B8G8R8A8, VK_FORMAT_B8G8R8A8_UNORM);
	SetComponentMapping(PF_B8G8R8A8, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_G8, VK_FORMAT_R8_UNORM);
	SetComponentMapping(PF_G8, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_G16, VK_FORMAT_R16_UNORM);
	SetComponentMapping(PF_G16, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_FloatRGB, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
	SetComponentMapping(PF_FloatRGB, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_FloatRGBA, VK_FORMAT_R16G16B16A16_SFLOAT, 8);
	SetComponentMapping(PF_FloatRGBA, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_DepthStencil, VK_FORMAT_D32_SFLOAT_S8_UINT);
	if (!GPixelFormats[PF_DepthStencil].Supported)
	{
		MapFormatSupport(PF_DepthStencil, VK_FORMAT_D24_UNORM_S8_UINT);
		if (!GPixelFormats[PF_DepthStencil].Supported)
		{
			MapFormatSupport(PF_DepthStencil, VK_FORMAT_D16_UNORM_S8_UINT);
			if (!GPixelFormats[PF_DepthStencil].Supported)
			{
				UE_LOG(LogVulkanRHI, Error, TEXT("No stencil texture format supported!"));
			}
		}
	}
	SetComponentMapping(PF_DepthStencil, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY);

	MapFormatSupport(PF_ShadowDepth, VK_FORMAT_D16_UNORM);
	SetComponentMapping(PF_ShadowDepth, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY);

	// Requirement for GPU particles
	MapFormatSupport(PF_G32R32F, VK_FORMAT_R32G32_SFLOAT, 8);
	SetComponentMapping(PF_G32R32F, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_A32B32G32R32F, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
	SetComponentMapping(PF_A32B32G32R32F, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_G16R16, VK_FORMAT_R16G16_UNORM);
	SetComponentMapping(PF_G16R16, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_G16R16F, VK_FORMAT_R16G16_SFLOAT);
	SetComponentMapping(PF_G16R16F, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_G16R16F_FILTER, VK_FORMAT_R16G16_SFLOAT);
	SetComponentMapping(PF_G16R16F_FILTER, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R16_UINT, VK_FORMAT_R16_UINT);
	SetComponentMapping(PF_R16_UINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R16_SINT, VK_FORMAT_R16_SINT);
	SetComponentMapping(PF_R16_SINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R32_UINT, VK_FORMAT_R32_UINT);
	SetComponentMapping(PF_R32_UINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R32_SINT, VK_FORMAT_R32_SINT);
	SetComponentMapping(PF_R32_SINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R8_UINT, VK_FORMAT_R8_UINT);
	SetComponentMapping(PF_R8_UINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_D24, VK_FORMAT_X8_D24_UNORM_PACK32);
	if (!GPixelFormats[PF_D24].Supported)
	{
		MapFormatSupport(PF_D24, VK_FORMAT_D24_UNORM_S8_UINT);
		if (!GPixelFormats[PF_D24].Supported)
		{
			MapFormatSupport(PF_D24, VK_FORMAT_D16_UNORM_S8_UINT);
			if (!GPixelFormats[PF_D24].Supported)
			{
				MapFormatSupport(PF_D24, VK_FORMAT_D32_SFLOAT);
				if (!GPixelFormats[PF_D24].Supported)
				{
					MapFormatSupport(PF_D24, VK_FORMAT_D32_SFLOAT_S8_UINT);
					if (!GPixelFormats[PF_D24].Supported)
					{
						MapFormatSupport(PF_D24, VK_FORMAT_D16_UNORM);
					}
				}
			}
		}
	}

	SetComponentMapping(PF_D24, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R16F, VK_FORMAT_R16_SFLOAT);
	SetComponentMapping(PF_R16F, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R16F_FILTER, VK_FORMAT_R16_SFLOAT);
	SetComponentMapping(PF_R16F_FILTER, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_FloatR11G11B10, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4);
	SetComponentMapping(PF_FloatR11G11B10, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_A2B10G10R10, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4);
	SetComponentMapping(PF_A2B10G10R10, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_A16B16G16R16, VK_FORMAT_R16G16B16A16_UNORM, 8);
	SetComponentMapping(PF_A16B16G16R16, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_A8, VK_FORMAT_R8_UNORM);
	SetComponentMapping(PF_A8, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R);

	MapFormatSupport(PF_R5G6B5_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16);
	SetComponentMapping(PF_R5G6B5_UNORM, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_R8G8B8A8, VK_FORMAT_R8G8B8A8_UNORM);
	SetComponentMapping(PF_R8G8B8A8, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_R16G16_UINT, VK_FORMAT_R16G16_UINT);
	SetComponentMapping(PF_R16G16_UINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT);
	SetComponentMapping(PF_R16G16B16A16_UINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT);
	SetComponentMapping(PF_R16G16B16A16_SINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_R32G32B32A32_UINT, VK_FORMAT_R32G32B32_UINT);
	SetComponentMapping(PF_R32G32B32A32_UINT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_R8G8, VK_FORMAT_R8G8_UNORM);
	SetComponentMapping(PF_R8G8, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_V8U8, VK_FORMAT_R8G8_UNORM);
	SetComponentMapping(PF_V8U8, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

	MapFormatSupport(PF_R32_FLOAT, VK_FORMAT_R32_SFLOAT);
	SetComponentMapping(PF_R32_FLOAT, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO);

#if PLATFORM_DESKTOP
	MapFormatSupport(PF_DXT1, VK_FORMAT_BC1_RGB_UNORM_BLOCK);	// Also what OpenGL expects (RGBA instead RGB, but not SRGB)
	SetComponentMapping(PF_DXT1, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE);

	MapFormatSupport(PF_DXT3, VK_FORMAT_BC2_UNORM_BLOCK);
	SetComponentMapping(PF_DXT3, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_DXT5, VK_FORMAT_BC3_UNORM_BLOCK);
	SetComponentMapping(PF_DXT5, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_BC4, VK_FORMAT_BC4_UNORM_BLOCK);
	SetComponentMapping(PF_BC4, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_BC5, VK_FORMAT_BC5_UNORM_BLOCK);
	SetComponentMapping(PF_BC5, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_BC6H, VK_FORMAT_BC6H_UFLOAT_BLOCK);
	SetComponentMapping(PF_BC6H, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);

	MapFormatSupport(PF_BC7, VK_FORMAT_BC7_UNORM_BLOCK);
	SetComponentMapping(PF_BC7, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
#elif PLATFORM_ANDROID
	MapFormatSupport(PF_ASTC_4x4, VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
	if (GPixelFormats[PF_ASTC_4x4].Supported)
	{
		SetComponentMapping(PF_ASTC_4x4, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
	}

	MapFormatSupport(PF_ASTC_6x6, VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
	if (GPixelFormats[PF_ASTC_6x6].Supported)
	{
		SetComponentMapping(PF_ASTC_6x6, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
	}

	MapFormatSupport(PF_ASTC_8x8, VK_FORMAT_ASTC_8x8_UNORM_BLOCK);
	if (GPixelFormats[PF_ASTC_8x8].Supported)
	{
		SetComponentMapping(PF_ASTC_8x8, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
	}

	MapFormatSupport(PF_ASTC_10x10, VK_FORMAT_ASTC_10x10_UNORM_BLOCK);
	if (GPixelFormats[PF_ASTC_10x10].Supported)
	{
		SetComponentMapping(PF_ASTC_10x10, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
	}

	MapFormatSupport(PF_ASTC_12x12, VK_FORMAT_ASTC_12x12_UNORM_BLOCK);
	if (GPixelFormats[PF_ASTC_12x12].Supported)
	{
		SetComponentMapping(PF_ASTC_12x12, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
	}

	// ETC1 is a subset of ETC2 R8G8B8.
	MapFormatSupport(PF_ETC1, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
	if (GPixelFormats[PF_ETC1].Supported)
	{
		SetComponentMapping(PF_ETC1, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE);
	}
	
	MapFormatSupport(PF_ETC2_RGB, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
	if (GPixelFormats[PF_ETC2_RGB].Supported)
	{
		SetComponentMapping(PF_ETC2_RGB, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE);
	}

	MapFormatSupport(PF_ETC2_RGBA, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
	if (GPixelFormats[PF_ETC2_RGB].Supported)
	{
		SetComponentMapping(PF_ETC2_RGBA, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
	}
#endif
}

void FVulkanDevice::MapFormatSupport(EPixelFormat UEFormat, VkFormat VulkanFormat)
{
	FPixelFormatInfo& FormatInfo = GPixelFormats[UEFormat];
	FormatInfo.PlatformFormat = VulkanFormat;
	FormatInfo.Supported = IsFormatSupported(VulkanFormat);

	if(!FormatInfo.Supported)
	{
		UE_LOG(LogVulkanRHI, Warning, TEXT("EPixelFormat(%d) is not supported with Vk format %d"), (int32)UEFormat, (int32)VulkanFormat);
	}
}

void FVulkanDevice::SetComponentMapping(EPixelFormat UEFormat, VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b, VkComponentSwizzle a)
{
	// Please ensure that we support the mapping, otherwise there is no point setting it.
	check(GPixelFormats[UEFormat].Supported);
	VkComponentMapping& ComponentMapping = PixelFormatComponentMapping[UEFormat];
	ComponentMapping.r = r;
	ComponentMapping.g = g;
	ComponentMapping.b = b;
	ComponentMapping.a = a;
}

void FVulkanDevice::MapFormatSupport(EPixelFormat UEFormat, VkFormat VulkanFormat, int32 BlockBytes)
{
	MapFormatSupport(UEFormat, VulkanFormat);
	FPixelFormatInfo& FormatInfo = GPixelFormats[UEFormat];
	FormatInfo.BlockBytes = BlockBytes;
}

bool FVulkanDevice::QueryGPU(int32 DeviceIndex)
{
	bool bDiscrete = false;
	VulkanRHI::vkGetPhysicalDeviceProperties(Gpu, &GpuProps);

	auto GetDeviceTypeString = [&]()
	{
		FString Info;
		switch (GpuProps.deviceType)
		{
		case  VK_PHYSICAL_DEVICE_TYPE_OTHER:
			Info = TEXT("Other");
			break;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			Info = TEXT("Integrated GPU");
			break;
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			Info = TEXT("Discrete GPU");
			bDiscrete = true;
			break;
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
			Info = TEXT("Virtual GPU");
			break;
		case VK_PHYSICAL_DEVICE_TYPE_CPU:
			Info = TEXT("CPU");
			break;
		default:
			Info = TEXT("Unknown");
			break;
		}
		return Info;
	};

	UE_LOG(LogVulkanRHI, Display, TEXT("Initializing Device %d"), DeviceIndex);
	UE_LOG(LogVulkanRHI, Display, TEXT("API 0x%x Driver 0x%x VendorId 0x%x"), GpuProps.apiVersion, GpuProps.driverVersion, GpuProps.vendorID);
	UE_LOG(LogVulkanRHI, Display, TEXT("Name %s Device 0x%x Type %s"), ANSI_TO_TCHAR(GpuProps.deviceName), GpuProps.deviceID, *GetDeviceTypeString());
	UE_LOG(LogVulkanRHI, Display, TEXT("Max Descriptor Sets Bound %d Timestamps %d"), GpuProps.limits.maxBoundDescriptorSets, GpuProps.limits.timestampComputeAndGraphics);

	uint32 QueueCount = 0;
	VulkanRHI::vkGetPhysicalDeviceQueueFamilyProperties(Gpu, &QueueCount, nullptr);
	check(QueueCount >= 1);

	QueueFamilyProps.AddUninitialized(QueueCount);
	VulkanRHI::vkGetPhysicalDeviceQueueFamilyProperties(Gpu, &QueueCount, QueueFamilyProps.GetData());

	return bDiscrete;
}

void FVulkanDevice::InitGPU(int32 DeviceIndex)
{
	// Query features
	VulkanRHI::vkGetPhysicalDeviceFeatures(Gpu, &Features);

	UE_LOG(LogVulkanRHI, Display, TEXT("Geometry %d Tessellation %d"), Features.geometryShader, Features.tessellationShader);

	CreateDevice();

	SetupFormats();

	MemoryManager.Init(this);

	ResourceHeapManager.Init();

	FenceManager.Init(this);

	StagingManager.Init(this, GfxQueue);

	PipelineStateCache = new FVulkanPipelineStateCache(this);

	TArray<FString> CacheFilenames;
	if (PLATFORM_ANDROID)
	{
		CacheFilenames.Add(FPaths::ProjectDir() / TEXT("Build") / TEXT("ShaderCaches") / TEXT("Android") / TEXT("VulkanPSO.cache"));
	}
	CacheFilenames.Add(FPaths::ProjectSavedDir() / TEXT("VulkanPSO.cache"));	

	bool bSupportsTimestamps = (GpuProps.limits.timestampComputeAndGraphics == VK_TRUE);
	if (bSupportsTimestamps)
	{
		check(!TimestampQueryPool);
		TimestampQueryPool = new FVulkanTimestampPool(this, NUM_TIMESTAMP_QUERIES_PER_POOL);
	}
	else
	{
		UE_LOG(LogVulkanRHI, Warning, TEXT("Timestamps not supported on Device"));
	}

	ImmediateContext = new FVulkanCommandListContext((FVulkanDynamicRHI*)GDynamicRHI, this, true);

	PipelineStateCache->InitAndLoad(CacheFilenames);

	// Setup default resource
	{
		FSamplerStateInitializerRHI Default(SF_Point);
		DefaultSampler = new FVulkanSamplerState(Default, *this);
	}
}

void FVulkanDevice::PrepareForDestroy()
{
	WaitUntilIdle();
}

void FVulkanDevice::Destroy()
{
	delete ImmediateContext;
	ImmediateContext = nullptr;

	if (TimestampQueryPool)
	{
		TimestampQueryPool->Destroy();
		delete TimestampQueryPool;
	}

	for (FVulkanQueryPool* QueryPool : OcclusionQueryPools)
	{
		QueryPool->Destroy();
		delete QueryPool;
	}
	OcclusionQueryPools.SetNum(0, false);

	delete PipelineStateCache;
	PipelineStateCache = nullptr;

	delete DefaultSampler;
	DefaultSampler = nullptr;

	StagingManager.Deinit();

	ResourceHeapManager.Deinit();

	delete TransferQueue;
	delete GfxQueue;

	FenceManager.Deinit();

	MemoryManager.Deinit();

	DeferredDeletionQueue.Clear();

	VulkanRHI::vkDestroyDevice(Device, nullptr);
	Device = VK_NULL_HANDLE;
}

void FVulkanDevice::WaitUntilIdle()
{
	VERIFYVULKANRESULT(VulkanRHI::vkDeviceWaitIdle(Device));

	//#todo-rco: Loop through all contexts!
	GetImmediateContext().GetCommandBufferManager()->RefreshFenceStatus();
}

bool FVulkanDevice::IsFormatSupported(VkFormat Format) const
{
	auto ArePropertiesSupported = [](const VkFormatProperties& Prop) -> bool
	{
		return (Prop.bufferFeatures != 0) || (Prop.linearTilingFeatures != 0) || (Prop.optimalTilingFeatures != 0);
	};

	if (Format >= 0 && Format < VK_FORMAT_RANGE_SIZE)
	{
		const VkFormatProperties& Prop = FormatProperties[Format];
		return ArePropertiesSupported(Prop);
	}

	// Check for extension formats
	const VkFormatProperties* FoundProperties = ExtensionFormatProperties.Find(Format);
	if (FoundProperties)
	{
		return ArePropertiesSupported(*FoundProperties);
	}

	// Add it for faster caching next time
	VkFormatProperties& NewProperties = ExtensionFormatProperties.Add(Format);
	FMemory::Memzero(NewProperties);
	VulkanRHI::vkGetPhysicalDeviceFormatProperties(Gpu, Format, &NewProperties);

	return ArePropertiesSupported(NewProperties);
}

const VkComponentMapping& FVulkanDevice::GetFormatComponentMapping(EPixelFormat UEFormat) const
{
	check(GPixelFormats[UEFormat].Supported);
	return PixelFormatComponentMapping[UEFormat];
}

void FVulkanDevice::NotifyDeletedRenderTarget(VkImage Image)
{
	//#todo-rco: Loop through all contexts!
	GetImmediateContext().NotifyDeletedRenderTarget(Image);
}

void FVulkanDevice::PrepareForCPURead()
{
	//#todo-rco: Process other contexts first!
	ImmediateContext->PrepareForCPURead();
}

void FVulkanDevice::SubmitCommandsAndFlushGPU()
{
	//#todo-rco: Process other contexts first!

	FVulkanCommandBufferManager* CmdMgr = ImmediateContext->GetCommandBufferManager();
	if (CmdMgr->HasPendingUploadCmdBuffer())
	{
		CmdMgr->SubmitUploadCmdBuffer(true);
	}
	if (CmdMgr->HasPendingActiveCmdBuffer())
	{
		//#todo-rco: If we get real render passes then this is not needed
		if (ImmediateContext->TransitionState.CurrentRenderPass)
		{
			ImmediateContext->TransitionState.EndRenderPass(CmdMgr->GetActiveCmdBuffer());
		}

		CmdMgr->SubmitActiveCmdBuffer(true);
	}
	CmdMgr->PrepareForNewActiveCommandBuffer();
}

void FVulkanDevice::NotifyDeletedGfxPipeline(class FVulkanGraphicsPipelineState* Pipeline)
{
	//#todo-rco: Loop through all contexts!
	if (ImmediateContext)
	{
		ImmediateContext->PendingGfxState->NotifyDeletedPipeline(Pipeline);
	}
}

void FVulkanDevice::NotifyDeletedComputePipeline(class FVulkanComputePipeline* Pipeline)
{
	//#todo-rco: Loop through all contexts!
	if (ImmediateContext)
	{
		ImmediateContext->PendingComputeState->NotifyDeletedPipeline(Pipeline);
	}
}
