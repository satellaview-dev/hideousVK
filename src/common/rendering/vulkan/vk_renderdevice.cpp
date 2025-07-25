/*
**  Vulkan backend
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include <zvulkan/vulkanobjects.h>

#include <inttypes.h>

#include "v_video.h"
#include "m_png.h"

#include "r_videoscale.h"
#include "i_time.h"
#include "v_text.h"
#include "version.h"
#include "v_draw.h"

#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_cvars.h"
#include "hw_skydome.h"
#include "flatvertices.h"
#include "hw_meshbuilder.h"

#include "vk_renderdevice.h"
#include "vulkan/vk_renderstate.h"
#include "vulkan/vk_postprocess.h"
#include "vulkan/vk_levelmesh.h"
#include "vulkan/vk_lightmapper.h"
#include "vulkan/vk_lightprober.h"
#include "vulkan/pipelines/vk_renderpass.h"
#include "vulkan/descriptorsets/vk_descriptorset.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/shaders/vk_shadercache.h"
#include "vulkan/samplers/vk_samplers.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/framebuffers/vk_framebuffer.h"
#include "vulkan/commands/vk_commandbuffer.h"
#include "vulkan/buffers/vk_hwbuffer.h"
#include "vulkan/buffers/vk_buffer.h"
#include "vulkan/buffers/vk_rsbuffers.h"
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkansurface.h>
#include <zvulkan/vulkancompatibledevice.h>
#include "engineerrors.h"
#include "c_dispatch.h"
#include "menu.h"
#include "cmdlib.h"

FString JitCaptureStackTrace(int framesToSkip, bool includeNativeFrames, int maxFrames = -1);

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Bool, r_skipmats)

// Physical device info
static std::vector<VulkanCompatibleDevice> SupportedDevices;
int vkversion;
static TArray<FString> memheapnames;
static TArray<VmaBudget> membudgets;
static int hwtexturecount;

CUSTOM_CVAR(Bool, vk_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CVAR(Bool, vk_debug_callstack, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vk_amd_driver_check, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, vk_device, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CUSTOM_CVAR(Bool, vk_rayquery, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vk_listdevices)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		Printf("#%d - %s\n", (int)i, SupportedDevices[i].Device->Properties.Properties.deviceName);
	}
}

CCMD(vk_membudget)
{
	for (size_t i = 0; i < membudgets.size(); i++)
	{
		if (membudgets[i].budget != 0)
		{
			Printf("#%d%s - %d MB used out of %d MB estimated budget (%d%%)\n",
				(int)i, memheapnames[i].GetChars(),
				(int)(membudgets[i].usage / (1024 * 1024)),
				(int)(membudgets[i].budget / (1024 * 1024)),
				(int)(membudgets[i].usage * 100 / membudgets[i].budget));
		}
		else
		{
			Printf("#%d %s - %d MB used\n",
				(int)i, memheapnames[i].GetChars(),
				(int)(membudgets[i].usage / (1024 * 1024)));
		}
	}
	Printf("%d total hardware textures\n", hwtexturecount);
}

void I_BuildVKDeviceList(FOptionValues* opt)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		unsigned int idx = opt->mValues.Reserve(1);
		opt->mValues[idx].Value = (double)i;
		opt->mValues[idx].Text = SupportedDevices[i].Device->Properties.Properties.deviceName;
	}
}

void VulkanError(const char* text)
{
	throw CVulkanError(text);
}

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	bool showcallstack = strstr(typestr, "error") != nullptr;

	if (showcallstack)
		Printf("\n");

	Printf(TEXTCOLOR_RED "[%s] ", typestr);
	Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());

	if (vk_debug_callstack && showcallstack)
	{
		FString callstack = JitCaptureStackTrace(0, true, 5);
		if (!callstack.IsEmpty())
			Printf("%s\n", callstack.GetChars());
	}
}

VulkanRenderDevice::VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanInstance> instance, std::shared_ptr<VulkanSurface> surface) : SystemBaseFrameBuffer(hMonitor, fullscreen)
{
	VulkanDeviceBuilder builder;
	builder.OptionalRayQuery();
	builder.RequireExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
	if (surface)
	{
		HasSurface = true;
		builder.Surface(surface);
	}
	builder.SelectDevice(vk_device);
	SupportedDevices = builder.FindDevices(instance);
	mDevice = builder.Create(instance);

	bool supportsBindless =
		mDevice->EnabledFeatures.DescriptorIndexing.descriptorBindingPartiallyBound &&
		mDevice->EnabledFeatures.DescriptorIndexing.runtimeDescriptorArray &&
		mDevice->EnabledFeatures.DescriptorIndexing.shaderSampledImageArrayNonUniformIndexing;
	if (!supportsBindless)
	{
		I_FatalError("This GPU does not support the minimum requirements of this application");
	}

	mUseRayQuery = vk_rayquery && mDevice->SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME) && mDevice->PhysicalDevice.Features.RayQuery.rayQuery;

	if (vk_amd_driver_check)
	{
		// While we found a workaround for the SPIR-V compiler crashing on specialization constants with rayquery,
		// the AMDVLK driver (but not the Mesa one!) now produces a shader that only the FIRST frame runs for 10
		// seconds. This produces a device lost on Windows (command buffer killed by OS) and the freeze from hell
		// on Linux.
		//
		// Maybe some day AMD will have a driver that works for us. Until that day their hardware gets demoted to
		// the legacy path without RT cores, sorry.
		auto& props = mDevice->PhysicalDevice.Properties.Properties;
		if (props.vendorID == 0x1002 && VK_VERSION_MAJOR(props.driverVersion) < 10)
		{
			if (mUseRayQuery)
			{
				Printf("AMD driver detected. Disabling RT cores. You can force RT cores on by setting vk_amd_driver_check to false.\n");
				mUseRayQuery = false;
			}
		}
	}

	mShaderCache = std::make_unique<VkShaderCache>(this);
}

VulkanRenderDevice::~VulkanRenderDevice()
{
	vkDeviceWaitIdle(mDevice->device); // make sure the GPU is no longer using any objects before RAII tears them down

	delete mSkyData;
	delete mShadowMap;

	if (mDescriptorSetManager)
		mDescriptorSetManager->Deinit();
	if (mCommands)
		mCommands->DeleteFrameObjects();
	if (mTextureManager)
		mTextureManager->Deinit();
	if (mBufferManager)
		mBufferManager->Deinit();
	if (mShaderManager)
		mShaderManager->Deinit();
	if (mCommands)
		mCommands->DeleteFrameObjects();
}

bool VulkanRenderDevice::SupportsRenderTargetFormat(VkFormat format)
{
	if (ImageBuilder()
		.Size(1024, 1024)
		.Format(format)
		.Usage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.IsFormatSupported(GetDevice()))
		return true;

	return ImageBuilder()
		.Size(1024, 1024)
		.Format(format)
		.Samples(VK_SAMPLE_COUNT_4_BIT)
		.Usage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.IsFormatSupported(GetDevice());
}

bool VulkanRenderDevice::SupportsNormalGBufferFormat(VkFormat format)
{
	if (ImageBuilder()
		.Size(1024, 1024)
		.Format(format)
		.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.IsFormatSupported(GetDevice()))
		return true;

	return ImageBuilder()
		.Size(1024, 1024)
		.Format(format)
		.Samples(VK_SAMPLE_COUNT_4_BIT)
		.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.IsFormatSupported(GetDevice());
}

void VulkanRenderDevice::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	// Use the same names here as OpenGL returns.
	switch (mDevice->PhysicalDevice.Properties.Properties.vendorID)
	{
	case 0x1002: vendorstring = "ATI Technologies Inc.";     break;
	case 0x10DE: vendorstring = "NVIDIA Corporation";  break;
	case 0x8086: vendorstring = "Intel";   break;
	default:     vendorstring = "Unknown"; break;
	}

	uniformblockalignment = (unsigned int)mDevice->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment;
	maxuniformblock = std::min(mDevice->PhysicalDevice.Properties.Properties.limits.maxUniformBufferRange, (uint32_t)1024 * 1024);

	if (SupportsRenderTargetFormat(VK_FORMAT_D24_UNORM_S8_UINT))
	{
		DepthStencilFormat = VK_FORMAT_D24_UNORM_S8_UINT;
	}
	else if (SupportsRenderTargetFormat(VK_FORMAT_D32_SFLOAT_S8_UINT))
	{
		DepthStencilFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
	}
	else
	{
		I_FatalError("This device does not support any of the required depth stencil image formats.");
	}

	if (SupportsNormalGBufferFormat(VK_FORMAT_A2R10G10B10_UNORM_PACK32))
	{
		NormalFormat = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	}
	else if (SupportsNormalGBufferFormat(VK_FORMAT_R8G8B8A8_UNORM))
	{
		NormalFormat = VK_FORMAT_R8G8B8A8_UNORM;
	}
	else
	{
		I_FatalError("This device does not support any of the required normal buffer image formats.");
	}

	NullMesh.reset(new LevelMesh());
	levelMesh = NullMesh.get();

	mCommands.reset(new VkCommandBufferManager(this));

	mSamplerManager.reset(new VkSamplerManager(this));
	mTextureManager.reset(new VkTextureManager(this));
	mFramebufferManager.reset(new VkFramebufferManager(this));
	mBufferManager.reset(new VkBufferManager(this));

	mScreenBuffers.reset(new VkRenderBuffers(this));
	mSaveBuffers.reset(new VkRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();

	mPostprocess.reset(new VkPostprocess(this));
	mDescriptorSetManager.reset(new VkDescriptorSetManager(this));
	mShaderManager.reset(new VkShaderManager(this));
	mRenderPassManager.reset(new VkRenderPassManager(this));
	mLevelMesh.reset(new VkLevelMesh(this));
	mLightmapper.reset(new VkLightmapper(this));
	mLightprober.reset(new VkLightprober(this));

	mBufferManager->Init();

	mSkyData = new FSkyVertexBuffer(this);
	mShadowMap = new ShadowMap(this);

	mDescriptorSetManager->Init();

	mRenderState = std::make_unique<VkRenderState>(this);
}

void VulkanRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	GetPostprocess()->SetActiveRenderTarget();

	Draw2D();
	twod->Clear();

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	Flush3D.Unclock();

	mCommands->WaitForCommands(true);
	mCommands->UpdateGpuStats();

	SystemBaseFrameBuffer::Update();
}

bool VulkanRenderDevice::CompileNextShader()
{
	return mShaderManager->CompileNextShader();
}

void VulkanRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	auto BaseLayer = static_cast<VkHardwareTexture*>(tex->GetHardwareTexture(0, 0));

	VkTextureImage *image = BaseLayer->GetImage(tex, 0, 0);
	VkTextureImage *depthStencil = BaseLayer->GetDepthStencil(tex);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(image, depthStencil->View.get(), image->Image->width, image->Image->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = min(tex->GetWidth(), image->Image->width);
	bounds.height = min(tex->GetHeight(), image->Image->height);

	renderFunc(bounds);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples());

	tex->SetUpdated(true);
}

void VulkanRenderDevice::ResetLightProbes()
{
	mTextureManager->ResetLightProbes();
}

void VulkanRenderDevice::RenderLightProbe(int probeIndex, std::function<void(IntRect& bounds, int side)> renderFunc)
{
	mLightprober->RenderEnvironmentMap(std::move(renderFunc));
	mLightprober->GenerateIrradianceMap(probeIndex);
	mLightprober->GeneratePrefilterMap(probeIndex);
}

void VulkanRenderDevice::EndLightProbePass()
{
	mLightprober->EndLightProbePass();
}

void VulkanRenderDevice::DownloadLightProbes(int probeCount, TArrayView<uint16_t> irradianceMaps, TArrayView<uint16_t> prefilterMaps)
{
	mTextureManager->DownloadIrradiancemap(probeCount, irradianceMaps);
	mTextureManager->DownloadPrefiltermap(probeCount, prefilterMaps);
}

void VulkanRenderDevice::UploadLightProbes(int probeCount, const TArray<uint16_t>& irradianceMaps, const TArray<uint16_t>& prefilterMaps)
{
	mTextureManager->UploadIrradiancemap(probeCount, irradianceMaps);
	mTextureManager->UploadPrefiltermap(probeCount, prefilterMaps);
}

void VulkanRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, bool palettePostprocess, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) mPostprocess->BlitSceneToPostprocess(); // Copy the resulting scene to the current post process texture
	mPostprocess->PostProcessScene(fixedcm, flash, palettePostprocess, afterBloomDrawEndScene2D);
}

const char* VulkanRenderDevice::DeviceName() const
{
	const auto &props = mDevice->PhysicalDevice.Properties;
	return props.Properties.deviceName;
}

void VulkanRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;
}

void VulkanRenderDevice::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	MaterialLayerInfo* layer;

	auto systex = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation, &layer));
	systex->GetImage(layer->layerTexture, translation, layer->scaleFlags);

	int numLayers = mat->NumLayers();
	for (int i = 1; i < numLayers; i++)
	{
		auto syslayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
	}
}

IHardwareTexture *VulkanRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new VkHardwareTexture(this, numchannels);
}

FMaterial* VulkanRenderDevice::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new VkMaterial(this, tex, scaleflags);
}

IBuffer*VulkanRenderDevice::CreateVertexBuffer(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute* attrs)
{
	return GetBufferManager()->CreateVertexBuffer(numBindingPoints, numAttributes, stride, attrs);
}

IBuffer*VulkanRenderDevice::CreateIndexBuffer()
{
	return GetBufferManager()->CreateIndexBuffer();
}

void VulkanRenderDevice::SetTextureFilterMode()
{
	if (mSamplerManager)
	{
		mDescriptorSetManager->ResetHWTextureSets();
		mSamplerManager->ResetHWSamplers();
	}
}

void VulkanRenderDevice::StartPrecaching()
{
	// Destroy the texture descriptors to avoid problems with potentially stale textures.
	mDescriptorSetManager->ResetHWTextureSets();
}

void VulkanRenderDevice::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void VulkanRenderDevice::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();

	mTextureManager->SetGamePalette();
}

FTexture *VulkanRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");

	return tex;
}

FTexture *VulkanRenderDevice::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");

	return tex;
}

void VulkanRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data)
{
	VkTextureImage image;

	// Convert from rgba16f to rgba8 using the GPU:
	image.Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.Size(w, h)
		.DebugName("CopyScreenToBuffer")
		.Create(mDevice.get());

	GetPostprocess()->BlitCurrentToImage(&image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Staging buffer for download
	auto staging = BufferBuilder()
		.Size(w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("CopyScreenToBuffer")
		.Create(mDevice.get());

	// Copy from image to buffer
	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	mCommands->GetDrawCommands()->copyImageToBuffer(image.Image->image, image.Layout, staging->buffer, 1, &region);

	// Submit command buffers and wait for device to finish the work
	mCommands->WaitForCommands(false);

	// Map and convert from rgba8 to rgb8
	uint8_t *dest = (uint8_t*)data;
	uint8_t *pixels = (uint8_t*)staging->Map(0, w * h * 4);
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		int sindex = (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			dest[dindex] = pixels[sindex];
			dest[dindex + 1] = pixels[sindex + 1];
			dest[dindex + 2] = pixels[sindex + 2];
			dindex += 3;
			sindex += 4;
		}
	}
	staging->Unmap();
}

void VulkanRenderDevice::SetActiveRenderTarget()
{
	mPostprocess->SetActiveRenderTarget();
}

TArray<uint8_t> VulkanRenderDevice::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = w;
	box.height = h;
	mPostprocess->DrawPresentTexture(box, true, true);

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return ScreenshotBuffer;
}

void VulkanRenderDevice::BeginFrame()
{
	vmaSetCurrentFrameIndex(mDevice->allocator, 0);
	membudgets.Resize(mDevice->PhysicalDevice.Properties.Memory.memoryHeapCount);
	vmaGetHeapBudgets(mDevice->allocator, membudgets.data());
	if (memheapnames.size() == 0)
	{
		memheapnames.Resize(mDevice->PhysicalDevice.Properties.Memory.memoryHeapCount);
		for (unsigned int i = 0; i < memheapnames.Size(); i++)
		{
			bool deviceLocal = !!(mDevice->PhysicalDevice.Properties.Memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
			memheapnames[i] = deviceLocal ? " (device local)" : "";
		}
	}
	hwtexturecount = mTextureManager->GetHWTextureCount();

	FrameTileUpdates = 0;

	GetRenderPassManager()->ProcessMainThreadTasks();
	GetTextureManager()->ProcessMainThreadTasks();

	if (levelMeshChanged)
	{
		levelMeshChanged = false;
		mLevelMesh->SetLevelMesh(levelMesh);
		GetTextureManager()->CreateLightmap(levelMesh->Lightmap.TextureSize, levelMesh->Lightmap.TextureCount, std::move(levelMesh->Lightmap.TextureData));
		GetLightmapper()->SetLevelMesh(levelMesh);
	}

	SetViewportRects(nullptr);
	mCommands->BeginFrame();
	mLevelMesh->BeginFrame();
	mTextureManager->BeginFrame(levelMesh->Lightmap.TextureSize, levelMesh->Lightmap.TextureCount);
	mScreenBuffers->BeginFrame(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
	mRenderState->BeginFrame();
	mDescriptorSetManager->BeginFrame();
	mLightmapper->BeginFrame();
}

void VulkanRenderDevice::Draw2D()
{
	::Draw2D(twod, *RenderState());
}

void VulkanRenderDevice::WaitForCommands(bool finish)
{
	mCommands->WaitForCommands(finish);
}

void VulkanRenderDevice::PrintStartupLog()
{
	const auto &props = mDevice->PhysicalDevice.Properties.Properties;

	FString deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType.Format("%d", (int)props.deviceType); break;
	}

	FString apiVersion, driverVersion;
	apiVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	driverVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
	vkversion = VK_API_VERSION_MAJOR(props.apiVersion) * 100 + VK_API_VERSION_MINOR(props.apiVersion);

	Printf("Vulkan device: " TEXTCOLOR_ORANGE "%s\n", props.deviceName);
	Printf("Vulkan device type: %s\n", deviceType.GetChars());
	Printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.GetChars(), driverVersion.GetChars());

	Printf(PRINT_LOG, "Vulkan extensions:");
	for (const VkExtensionProperties &p : mDevice->PhysicalDevice.Extensions)
	{
		Printf(PRINT_LOG, " %s", p.extensionName);
	}
	Printf(PRINT_LOG, "\n");

	const auto &limits = props.limits;
	Printf(PRINT_LOG, "Max. texture size: %d\n", limits.maxImageDimension2D);
	Printf(PRINT_LOG, "Max. uniform buffer range: %d\n", limits.maxUniformBufferRange);
	Printf(PRINT_LOG, "Min. uniform buffer offset alignment: %" PRIu64 "\n", limits.minUniformBufferOffsetAlignment);
}

void VulkanRenderDevice::SetLevelMesh(LevelMesh* mesh)
{
	if (!mesh) // Vulkan must have a mesh for its data structures in shaders to remain sane
	{
		NullMesh.reset(new LevelMesh()); // we must have a completely new mesh here as the upload ranges needs to reset as well
		mesh = NullMesh.get();
	}
	levelMesh = mesh;
	levelMeshChanged = true;
}

void VulkanRenderDevice::UpdateLightmaps(const TArray<LightmapTile*>& tiles)
{
	FrameTileUpdates += (int)tiles.size();
	GetLightmapper()->Raytrace(tiles);
}

void VulkanRenderDevice::SetShadowMaps(const TArray<float>& lights, hwrenderer::LevelAABBTree* tree, bool newTree)
{
	auto buffers = GetBufferManager();

	buffers->Shadowmap.Lights->SetData(sizeof(float) * lights.Size(), lights.Data(), BufferUsageType::Stream);

	if (newTree)
	{
		buffers->Shadowmap.Nodes->SetData(tree->NodesSize(), tree->Nodes(), BufferUsageType::Static);
		buffers->Shadowmap.Lines->SetData(tree->LinesSize(), tree->Lines(), BufferUsageType::Static);
	}
	else if (tree->Update())
	{
		buffers->Shadowmap.Nodes->SetSubData(tree->DynamicNodesOffset(), tree->DynamicNodesSize(), tree->DynamicNodes());
		buffers->Shadowmap.Lines->SetSubData(tree->DynamicLinesOffset(), tree->DynamicLinesSize(), tree->DynamicLines());
	}

	mPostprocess->UpdateShadowMap();
}

void VulkanRenderDevice::SetSaveBuffers(bool yes)
{
	if (yes) mActiveRenderBuffers = mSaveBuffers.get();
	else mActiveRenderBuffers = mScreenBuffers.get();
}

void VulkanRenderDevice::ImageTransitionScene(bool unknown)
{
	mPostprocess->ImageTransitionScene(unknown);
}

FRenderState* VulkanRenderDevice::RenderState()
{
	return mRenderState.get();
}

void VulkanRenderDevice::UpdateLinearDepthTexture()
{
	mPostprocess->UpdateLinearDepthTexture();
}

void VulkanRenderDevice::AmbientOccludeScene(float m5)
{
	mPostprocess->AmbientOccludeScene(m5);
}

void VulkanRenderDevice::SetSceneRenderTarget(bool useSSAO)
{
	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples());
}

void VulkanRenderDevice::DownloadLightmap(int arrayIndex, uint16_t* buffer)
{
	mTextureManager->DownloadLightmap(arrayIndex, buffer);
}

int VulkanRenderDevice::GetBindlessTextureIndex(FMaterial* material, int clampmode, int translation, bool paletteMode)
{
	GlobalShaderAddr addr;
	auto globalshader = GetGlobalShader(material->GetShaderIndex(), nullptr, addr);

	FMaterialState materialState;
	materialState.mMaterial = material;
	materialState.mClampMode = clampmode;
	materialState.mTranslation = translation;
	materialState.mPaletteMode = paletteMode;

	if(addr.type == 1 && *globalshader)
	{ // handle per-map global shaders
		materialState.globalShaderAddr = addr;
		materialState.mOverrideShader = globalshader->shaderindex;
	}

	return static_cast<VkMaterial*>(material)->GetBindlessIndex(materialState);
}

int VulkanRenderDevice::GetLevelMeshPipelineID(const MeshApplyData& applyData, const SurfaceUniforms& surfaceUniforms, const FMaterialState& material)
{
	if (levelVertexFormatIndex == -1)
	{
		static const std::vector<FVertexBufferAttribute> format =
		{
			{ 0, VATTR_VERTEX, VFmt_Float4, (int)myoffsetof(FFlatVertex, x) },
			{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FFlatVertex, u) },
			{ 0, VATTR_LIGHTMAP, VFmt_Float2, (int)myoffsetof(FFlatVertex, lu) },
			{ 1, VATTR_UNIFORM_INDEXES, VFmt_Int, 0 }
		};
		levelVertexFormatIndex = GetRenderPassManager()->GetVertexFormat({ sizeof(FFlatVertex), sizeof(int32_t) }, format);
	}

	VkPipelineKey pipelineKey;
	pipelineKey.DrawType = DT_Triangles;
	pipelineKey.RenderStyle = applyData.RenderStyle;
	pipelineKey.DepthFunc = applyData.DepthFunc;
	pipelineKey.ShaderKey.VertexFormat = levelVertexFormatIndex;
	if (applyData.SpecialEffect > EFF_NONE)
	{
		pipelineKey.ShaderKey.SpecialEffect = applyData.SpecialEffect;
		pipelineKey.ShaderKey.EffectState = 0;
		pipelineKey.ShaderKey.Layout.AlphaTest = false;
	}
	else
	{
		int effectState = material.mOverrideShader >= 0 ? material.mOverrideShader : (material.mMaterial ? material.mMaterial->GetShaderIndex() : 0);
		pipelineKey.ShaderKey.SpecialEffect = EFF_NONE;
		pipelineKey.ShaderKey.EffectState = applyData.TextureEnabled ? effectState : SHADER_NoTexture;
		if (r_skipmats && pipelineKey.ShaderKey.EffectState >= 3 && pipelineKey.ShaderKey.EffectState <= 4)
			pipelineKey.ShaderKey.EffectState = 0;
		pipelineKey.ShaderKey.Layout.AlphaTest = surfaceUniforms.uAlphaThreshold >= 0.f;
	}

	int tempTM = (material.mMaterial && material.mMaterial->Source()->isHardwareCanvas()) ? TM_OPAQUE : TM_NORMAL;
	int f = applyData.TextureModeFlags;
	if (!applyData.BrightmapEnabled) f &= ~(TEXF_Brightmap | TEXF_Glowmap);
	if (applyData.TextureClamp) f |= TEXF_ClampY;
	int uTextureMode = (applyData.TextureMode == TM_NORMAL && tempTM == TM_OPAQUE ? TM_OPAQUE : applyData.TextureMode) | f;

	pipelineKey.ShaderKey.TextureMode = uTextureMode & 0xffff;
	pipelineKey.ShaderKey.ClampY = (uTextureMode & TEXF_ClampY) != 0;
	pipelineKey.ShaderKey.Brightmap = (uTextureMode & TEXF_Brightmap) != 0;
	pipelineKey.ShaderKey.Detailmap = (uTextureMode & TEXF_Detailmap) != 0;
	pipelineKey.ShaderKey.Glowmap = (uTextureMode & TEXF_Glowmap) != 0;

	// The way GZDoom handles state is just plain insanity!
	int fogset = 0;
	if (applyData.FogEnabled)
	{
		if (applyData.FogEnabled == 2)
		{
			fogset = -3;	// 2D rendering with 'foggy' overlay.
		}
		else if (applyData.FogColor)
		{
			fogset = gl_fogmode;
		}
		else
		{
			fogset = -gl_fogmode;
		}
	}
	pipelineKey.ShaderKey.Simple2D = (fogset == -3);
	pipelineKey.ShaderKey.FogBeforeLights = (fogset > 0);
	pipelineKey.ShaderKey.FogAfterLights = (fogset < 0);
	pipelineKey.ShaderKey.FogRadial = (fogset < -1 || fogset > 1);
	pipelineKey.ShaderKey.SWLightRadial = (gl_fogmode == 2);
	pipelineKey.ShaderKey.SWLightBanded = false; // gl_bandedswlight;

	float lightlevel = surfaceUniforms.uLightLevel;
	if (lightlevel < 0.0)
	{
		pipelineKey.ShaderKey.LightMode = 0; // Default
	}
	else
	{
		/*if (mLightMode == 5)
			pipelineKey.ShaderKey.LightMode = 3; // Build
		else if (mLightMode == 16)
			pipelineKey.ShaderKey.LightMode = 2; // Vanilla
		else*/
			pipelineKey.ShaderKey.LightMode = 1; // Software
	}

	pipelineKey.ShaderKey.Layout.UseLevelMesh = true;

	for (unsigned int i = 0, count = levelMeshPipelineKeys.Size(); i < count; i++)
	{
		if (levelMeshPipelineKeys[i] == pipelineKey)
		{
			return i;
		}
	}

	levelMeshPipelineKeys.Push(pipelineKey);
	return levelMeshPipelineKeys.Size() - 1;
}

const VkPipelineKey& VulkanRenderDevice::GetLevelMeshPipelineKey(int id) const
{
	return levelMeshPipelineKeys[id];
}
