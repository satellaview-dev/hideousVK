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


#include "c_cvars.h"
#include "hw_material.h"
#include "hw_cvars.h"
#include "hw_renderstate.h"
#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/vk_renderdevice.h"
#include "vulkan/vk_postprocess.h"
#include "vulkan/commands/vk_commandbuffer.h"
#include "vulkan/samplers/vk_samplers.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/descriptorsets/vk_descriptorset.h"
#include "vulkan/shaders/vk_shader.h"
#include "vk_hwtexture.h"

CVAR(Bool, gl_async_textures, false, 0);

VkHardwareTexture::VkHardwareTexture(VulkanRenderDevice* fb, int numchannels) : fb(fb)
{
	mTexelsize = numchannels;
	fb->GetTextureManager()->AddTexture(this);
}

VkHardwareTexture::~VkHardwareTexture()
{
	if (fb)
		fb->GetTextureManager()->RemoveTexture(this);
}

void VkHardwareTexture::Reset()
{
	if (fb)
	{
		if (mappedSWFB)
		{
			mImage.Image->Unmap();
			mappedSWFB = nullptr;
		}

		mImage.Reset(fb);
		mPaletteImage.Reset(fb);
		mDepthStencil.Reset(fb);
	}
}

VkTextureImage *VkHardwareTexture::GetImage(FTexture *tex, int translation, int flags)
{
	if (flags & (CTF_Indexed | CTF_IndexedRedIsAlpha))
	{
		if (!mPaletteImage.Image)
			CreateImage(&mPaletteImage, tex, translation, flags);
		return &mPaletteImage;
	}
	else
	{
		if (!mImage.Image)
			CreateImage(&mImage, tex, translation, flags);
		return &mImage;
	}
}

VkTextureImage *VkHardwareTexture::GetDepthStencil(FTexture *tex)
{
	if (!mDepthStencil.View)
	{
		VkFormat format = fb->DepthStencilFormat;
		int w = tex->GetWidth();
		int h = tex->GetHeight();

		mDepthStencil.Image = ImageBuilder()
			.Size(w, h)
			.Samples(VK_SAMPLE_COUNT_1_BIT)
			.Format(format)
			.Usage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
			.DebugName("VkHardwareTexture.DepthStencil")
			.Create(fb->GetDevice());

		mDepthStencil.AspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

		mDepthStencil.View = ImageViewBuilder()
			.Image(mDepthStencil.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			.DebugName("VkHardwareTexture.DepthStencilView")
			.Create(fb->GetDevice());

		VkImageTransition()
			.AddImage(&mDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}
	return &mDepthStencil;
}

void VkHardwareTexture::CreateImage(VkTextureImage* image, FTexture *tex, int translation, int flags)
{
	if (!tex->isHardwareCanvas())
	{
		if (gl_async_textures && tex->GetImage())
		{
			// Create the texture now as that's easier to deal with elsewhere.

			FTextureBuffer texbuffer = tex->CreateTexBuffer(translation, flags | CTF_CheckOnly);
			bool indexed = flags & CTF_Indexed;
			CreateTexture(image, texbuffer.mWidth, texbuffer.mHeight, indexed ? 1 : 4, indexed ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM, texbuffer.mBuffer, !indexed);

			auto textureManager = fb->GetTextureManager();
			
			int uploadID = textureManager->CreateUploadID(this);
			textureManager->RunOnWorkerThread([=]() {

				// Load the texture on the worker thread
				auto imagedata = std::make_shared<FTextureBuffer>(tex->CreateTexBuffer(translation, flags | CTF_ProcessData));

				textureManager->RunOnMainThread([=]() {

					// Upload the texture on the main thread, as long as the hwrenderer didn't destroy this hwtexture already.
					if (textureManager->CheckUploadID(uploadID))
					{
						UploadTexture(image, imagedata->mWidth, imagedata->mHeight, indexed ? 1 : 4, indexed ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM, imagedata->mBuffer, !indexed);
					}

					});
			});
		}
		else
		{
			FTextureBuffer texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
			bool indexed = flags & CTF_Indexed;
			CreateTexture(image, texbuffer.mWidth, texbuffer.mHeight, indexed ? 1 : 4, indexed ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM, texbuffer.mBuffer, !indexed);
		}
	}
	else
	{
		VkFormat format = tex->IsHDR() ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
		int w = tex->GetWidth();
		int h = tex->GetHeight();

		image->Image = ImageBuilder()
			.Format(format)
			.Size(w, h)
			.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
			.DebugName("VkHardwareTexture.mImage")
			.Create(fb->GetDevice());

		image->View = ImageViewBuilder()
			.Image(image->Image.get(), format)
			.DebugName("VkHardwareTexture.mImageView")
			.Create(fb->GetDevice());

		VkImageTransition()
			.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}
}

void VkHardwareTexture::CreateTexture(VkTextureImage* image, int w, int h, int pixelsize, VkFormat format, const void *pixels, bool mipmap)
{
	if (w <= 0 || h <= 0)
		throw CVulkanError("Trying to create zero size texture");

	int totalSize = w * h * pixelsize;

	auto stagingBuffer = BufferBuilder()
		.Size(totalSize)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("VkHardwareTexture.mStagingBuffer")
		.Create(fb->GetDevice());

	uint8_t *data = (uint8_t*)stagingBuffer->Map(0, totalSize);

	if (pixels)
		memcpy(data, pixels, totalSize);
	else
		memset(data, 0, totalSize);

	stagingBuffer->Unmap();

	image->Image = ImageBuilder()
		.Format(format)
		.Size(w, h, !mipmap ? 1 : GetMipLevels(w, h))
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.DebugName("VkHardwareTexture.mImage")
		.Create(fb->GetDevice());

	image->View = ImageViewBuilder()
		.Image(image->Image.get(), format)
		.DebugName("VkHardwareTexture.mImageView")
		.Create(fb->GetDevice());

	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
		.Execute(cmdbuffer);

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.depth = 1;
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	cmdbuffer->copyBufferToImage(stagingBuffer->buffer, image->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	if (mipmap) image->GenerateMipmaps(cmdbuffer);

	// If we queued more than 64 MB of data already: wait until the uploads finish before continuing
	fb->GetCommands()->TransferDeleteList->Add(std::move(stagingBuffer));
	if (fb->GetCommands()->TransferDeleteList->TotalSize > 64 * 1024 * 1024)
		fb->GetCommands()->WaitForCommands(false, true);
}

void VkHardwareTexture::UploadTexture(VkTextureImage* image, int w, int h, int pixelsize, VkFormat format, const void* pixels, bool mipmap)
{
	if (w <= 0 || h <= 0)
		throw CVulkanError("Trying to create zero size texture");

	int totalSize = w * h * pixelsize;

	auto stagingBuffer = BufferBuilder()
		.Size(totalSize)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("VkHardwareTexture.mStagingBuffer")
		.Create(fb->GetDevice());

	uint8_t* data = (uint8_t*)stagingBuffer->Map(0, totalSize);
	memcpy(data, pixels, totalSize);
	stagingBuffer->Unmap();

	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
		.Execute(cmdbuffer);

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.depth = 1;
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	cmdbuffer->copyBufferToImage(stagingBuffer->buffer, image->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	if (mipmap) image->GenerateMipmaps(cmdbuffer);

	// If we queued more than 64 MB of data already: wait until the uploads finish before continuing
	fb->GetCommands()->TransferDeleteList->Add(std::move(stagingBuffer));
	if (fb->GetCommands()->TransferDeleteList->TotalSize > 64 * 1024 * 1024)
		fb->GetCommands()->WaitForCommands(false, true);
}

int VkHardwareTexture::GetMipLevels(int w, int h)
{
	int levels = 1;
	while (w > 1 || h > 1)
	{
		w = max(w >> 1, 1);
		h = max(h >> 1, 1);
		levels++;
	}
	return levels;
}

void VkHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (mImage.Image && (mImage.Image->width != w || mImage.Image->height != h || mTexelsize != texelsize))
	{
		Reset();
	}

	if (!mImage.Image)
	{
		VkFormat format = texelsize == 4 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8_UNORM;

		VkDeviceSize allocatedBytes = 0;
		mImage.Image = ImageBuilder()
			.Format(format)
			.Size(w, h)
			.LinearTiling()
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT, VMA_MEMORY_USAGE_UNKNOWN, VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
			.MemoryType(
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			.DebugName("VkHardwareTexture.mImage")
			.Create(fb->GetDevice(), &allocatedBytes);

		mTexelsize = texelsize;

		mImage.View = ImageViewBuilder()
			.Image(mImage.Image.get(), format)
			.DebugName("VkHardwareTexture.mImageView")
			.Create(fb->GetDevice());

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_GENERAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());

		bufferpitch = int(allocatedBytes / h / texelsize);
	}
}

uint8_t *VkHardwareTexture::MapBuffer()
{
	if (!mappedSWFB)
		mappedSWFB = (uint8_t*)mImage.Image->Map(0, mImage.Image->width * mImage.Image->height * mTexelsize);
	return mappedSWFB;
}

unsigned int VkHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name)
{
	// CreateTexture is used by the software renderer to create a screen output but without any screen data.
	if (buffer)
		CreateTexture(&mImage, w, h, mTexelsize, mTexelsize == 4 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8_UNORM, buffer, mipmap);
	return 0;
}

void VkHardwareTexture::CreateWipeTexture(int w, int h, const char *name)
{
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;

	mImage.Image = ImageBuilder()
		.Format(format)
		.Size(w, h)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
		.DebugName(name)
		.Create(fb->GetDevice());

	mTexelsize = 4;

	mImage.View = ImageViewBuilder()
		.Image(mImage.Image.get(), format)
		.DebugName(name)
		.Create(fb->GetDevice());

	if (fb->GetBuffers()->GetWidth() > 0 && fb->GetBuffers()->GetHeight() > 0)
	{
		fb->GetPostprocess()->BlitCurrentToImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	else
	{
		// hwrenderer asked image data from a frame buffer that was never written into. Let's give it that..
		// (ideally the hwrenderer wouldn't do this, but the calling code is too complex for me to fix)

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());

		VkImageSubresourceRange range = {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.layerCount = 1;
		range.levelCount = 1;

		VkClearColorValue value = {};
		value.float32[0] = 0.0f;
		value.float32[1] = 0.0f;
		value.float32[2] = 0.0f;
		value.float32[3] = 1.0f;
		fb->GetCommands()->GetTransferCommands()->clearColorImage(mImage.Image->image, mImage.Layout, &value, 1, &range);

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}
}

/////////////////////////////////////////////////////////////////////////////

VkMaterial::VkMaterial(VulkanRenderDevice* fb, FGameTexture* tex, int scaleflags) : FMaterial(tex, scaleflags), fb(fb)
{
	fb->GetDescriptorSetManager()->AddMaterial(this);
}

VkMaterial::~VkMaterial()
{
	if (fb)
		fb->GetDescriptorSetManager()->RemoveMaterial(this);
}

void VkMaterial::DeleteDescriptors()
{
	auto descriptors = fb->GetDescriptorSetManager();
	for (auto& set : mDescriptorSets)
	{
		descriptors->FreeBindlessSlot(set.bindlessIndex);
	}
	mDescriptorSets.clear();
}

int VkMaterial::GetBindlessIndex(const FMaterialState& state)
{
	return GetDescriptorEntry(state).bindlessIndex;
}

VkMaterial::DescriptorEntry& VkMaterial::GetDescriptorEntry(const FMaterialState& state)
{
	auto base = Source();
	int clampmode = state.mClampMode;
	int translation = state.mTranslation;
	GlobalShaderAddr globalShaderAddr = state.globalShaderAddr;
	auto translationp = IsLuminosityTranslation(translation)? translation : intptr_t(GPalette.GetTranslation(GetTranslationType(translation), GetTranslationIndex(translation)));

	clampmode = base->GetClampMode(clampmode);

	int paletteFlags = 0;
	if (state.mPaletteMode)
	{
		paletteFlags |= CTF_Indexed; // To do: may need to implement CTF_IndexedRedIsAlpha too for the "style.Flags & STYLEF_RedIsAlpha" case

		// We can't do linear filtering for indexed textures
		if (clampmode < CLAMP_NOFILTER)
			clampmode += CLAMP_NOFILTER;
	}

	for (auto& set : mDescriptorSets)
	{
		if (set.clampmode == clampmode && set.remap == translationp && set.globalShaderAddr == globalShaderAddr && set.indexed == state.mPaletteMode) return set;
	}

	const GlobalShaderDesc& globalshader = *GetGlobalShader(globalShaderAddr);
	int numLayersMat = globalshader ? NumNonMaterialLayers() : NumLayers();
	auto descriptors = fb->GetDescriptorSetManager();
	auto* sampler = fb->GetSamplerManager()->Get(clampmode);

	MaterialLayerInfo *layer = nullptr;
	auto systex = static_cast<VkHardwareTexture*>(GetLayer(0, state.mTranslation, &layer));

	// How many textures do we need?
	int textureCount;
	if (!(layer->scaleFlags & CTF_Indexed))
	{
		textureCount = numLayersMat;
		if (globalshader)
		{
			for (auto& texture : globalshader.CustomShaderTextures)
			{
				if (texture != nullptr)
					textureCount++;
			}
		}
	}
	else
	{
		textureCount = 3;
	}

	int bindlessIndex = descriptors->AllocBindlessSlot(textureCount);
	int texIndex = bindlessIndex;

	auto systeximage = systex->GetImage(layer->layerTexture, state.mTranslation, layer->scaleFlags | paletteFlags);
	descriptors->SetBindlessTexture(texIndex++, systeximage->View.get(), fb->GetSamplerManager()->Get(GetLayerFilter(0), clampmode));

	if (!(layer->scaleFlags & CTF_Indexed))
	{
		for (int i = 1; i < numLayersMat; i++)
		{
			auto syslayer = static_cast<VkHardwareTexture*>(GetLayer(i, 0, &layer));
			auto syslayerimage = syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags | paletteFlags);
			descriptors->SetBindlessTexture(texIndex++, syslayerimage->View.get(), fb->GetSamplerManager()->Get(GetLayerFilter(i), clampmode));
		}

		if(globalshader)
		{
			size_t i = 0;
			for (auto& texture : globalshader.CustomShaderTextures)
			{
				if (texture != nullptr)
				{
					VkHardwareTexture *tex = static_cast<VkHardwareTexture*>(texture.get()->GetHardwareTexture(0, 0));
					VkTextureImage *img = tex->GetImage(texture.get(), 0, paletteFlags);
					descriptors->SetBindlessTexture(texIndex++, img->View.get(), fb->GetSamplerManager()->Get(globalshader.CustomShaderTextureSampling[i], clampmode));
				}
				i++;
			}
		}
	}
	else
	{
		for (int i = 1; i < 3; i++)
		{
			auto syslayer = static_cast<VkHardwareTexture*>(GetLayer(i, translation, &layer));
			auto syslayerimage = syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags | paletteFlags);
			descriptors->SetBindlessTexture(texIndex++, syslayerimage->View.get(), fb->GetSamplerManager()->Get(GetLayerFilter(i), clampmode));
		}
	}

	if (texIndex != bindlessIndex + textureCount)
		I_FatalError("VkMaterial.GetDescriptorEntry: texIndex != bindlessIndex + textureCount");

	mDescriptorSets.emplace_back(clampmode, translationp, bindlessIndex, globalShaderAddr, state.mPaletteMode);
	return mDescriptorSets.back();
}
