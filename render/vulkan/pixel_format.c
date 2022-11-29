#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include "render/vulkan.h"

static const struct wlr_vk_format formats[] = {
	// Vulkan non-packed 8-bits-per-channel formats have an inverted channel
	// order compared to the DRM formats, because DRM format channel order
	// is little-endian while Vulkan format channel order is in memory byte
	// order.
	{
		.drm = DRM_FORMAT_R8,
		.vk = VK_FORMAT_R8_SRGB,
		.is_srgb = true,
	},
	{
		.drm = DRM_FORMAT_GR88,
		.vk = VK_FORMAT_R8G8_SRGB,
		.is_srgb = true,
	},
	{
		.drm = DRM_FORMAT_RGB888,
		.vk = VK_FORMAT_B8G8R8_SRGB,
		.is_srgb = true,
	},
	{
		.drm = DRM_FORMAT_BGR888,
		.vk = VK_FORMAT_R8G8B8_SRGB,
		.is_srgb = true,
	},
	{
		.drm = DRM_FORMAT_ARGB8888,
		.vk = VK_FORMAT_B8G8R8A8_SRGB,
		.is_srgb = true,
	},
	{
		.drm = DRM_FORMAT_XRGB8888,
		.vk = VK_FORMAT_B8G8R8A8_SRGB,
		.is_srgb = true,
	},
	{
		.drm = DRM_FORMAT_XBGR8888,
		.vk = VK_FORMAT_R8G8B8A8_SRGB,
		.is_srgb = true,
	},
	{
		.drm = DRM_FORMAT_ABGR8888,
		.vk = VK_FORMAT_R8G8B8A8_SRGB,
		.is_srgb = true,
	},

	// Vulkan packed formats have the same channel order as DRM formats on
	// little endian systems.
#if WLR_LITTLE_ENDIAN
	{
		.drm = DRM_FORMAT_RGBA4444,
		.vk = VK_FORMAT_R4G4B4A4_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_RGBX4444,
		.vk = VK_FORMAT_R4G4B4A4_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGRA4444,
		.vk = VK_FORMAT_B4G4R4A4_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGRX4444,
		.vk = VK_FORMAT_B4G4R4A4_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_RGB565,
		.vk = VK_FORMAT_R5G6B5_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGR565,
		.vk = VK_FORMAT_B5G6R5_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_RGBA5551,
		.vk = VK_FORMAT_R5G5B5A1_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_RGBX5551,
		.vk = VK_FORMAT_R5G5B5A1_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGRA5551,
		.vk = VK_FORMAT_B5G5R5A1_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_BGRX5551,
		.vk = VK_FORMAT_B5G5R5A1_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_ARGB1555,
		.vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_XRGB1555,
		.vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16,
	},
	{
		.drm = DRM_FORMAT_ARGB2101010,
		.vk = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
	},
	{
		.drm = DRM_FORMAT_XRGB2101010,
		.vk = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
	},
	{
		.drm = DRM_FORMAT_ABGR2101010,
		.vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
	},
	{
		.drm = DRM_FORMAT_XBGR2101010,
		.vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
	},
#endif

	// Vulkan 16-bits-per-channel formats have an inverted channel order
	// compared to DRM formats, just like the 8-bits-per-channel ones.
	// On little endian systems the memory representation of each channel
	// matches the DRM formats'.
#if WLR_LITTLE_ENDIAN
	{
		.drm = DRM_FORMAT_ABGR16161616,
		.vk = VK_FORMAT_R16G16B16A16_UNORM,
	},
	{
		.drm = DRM_FORMAT_XBGR16161616,
		.vk = VK_FORMAT_R16G16B16A16_UNORM,
	},
	{
		.drm = DRM_FORMAT_ABGR16161616F,
		.vk = VK_FORMAT_R16G16B16A16_SFLOAT,
	},
	{
		.drm = DRM_FORMAT_XBGR16161616F,
		.vk = VK_FORMAT_R16G16B16A16_SFLOAT,
	},
#endif
};

const struct wlr_vk_format *vulkan_get_format_list(size_t *len) {
	*len = sizeof(formats) / sizeof(formats[0]);
	return formats;
}

const struct wlr_vk_format *vulkan_get_format_from_drm(uint32_t drm_format) {
	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
		if (formats[i].drm == drm_format) {
			return &formats[i];
		}
	}
	return NULL;
}

static const VkImageUsageFlags render_usage =
	VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
static const VkImageUsageFlags tex_usage =
	VK_IMAGE_USAGE_SAMPLED_BIT |
	VK_IMAGE_USAGE_TRANSFER_DST_BIT;
static const VkImageUsageFlags dma_tex_usage =
	VK_IMAGE_USAGE_SAMPLED_BIT;

static const VkFormatFeatureFlags tex_features =
	VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
	VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	// NOTE: we don't strictly require this, we could create a NEAREST
	// sampler for formats that need it, in case this ever makes problems.
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
static const VkFormatFeatureFlags render_features =
	VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
static const VkFormatFeatureFlags dma_tex_features =
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	// NOTE: we don't strictly require this, we could create a NEAREST
	// sampler for formats that need it, in case this ever makes problems.
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

static bool query_modifier_usage_support(struct wlr_vk_device *dev, VkFormat vk_format,
		VkImageUsageFlags usage, const VkDrmFormatModifierPropertiesEXT *m,
		struct wlr_vk_format_modifier_props *out, const char **errmsg) {
	VkResult res;
	*errmsg = NULL;

	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modi = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.drmFormatModifier = m->drmFormatModifier,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkPhysicalDeviceExternalImageFormatInfo efmti = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.pNext = &modi,
	};
	VkPhysicalDeviceImageFormatInfo2 fmti = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.type = VK_IMAGE_TYPE_2D,
		.format = vk_format,
		.usage = usage,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.pNext = &efmti,
	};

	VkExternalImageFormatProperties efmtp = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
	};
	VkImageFormatProperties2 ifmtp = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = &efmtp,
	};
	const VkExternalMemoryProperties *emp = &efmtp.externalMemoryProperties;

	res = vkGetPhysicalDeviceImageFormatProperties2(dev->phdev, &fmti, &ifmtp);
	if (res != VK_SUCCESS) {
		if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
			*errmsg = "unsupported format";
		} else {
			wlr_vk_error("vkGetPhysicalDeviceImageFormatProperties2", res);
			*errmsg = "failed to get format properties";
		}
		return false;
	} else if (!(emp->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
		*errmsg = "import not supported";
		return false;
	}

	VkExtent3D me = ifmtp.imageFormatProperties.maxExtent;
	*out = (struct wlr_vk_format_modifier_props){
		.props = *m,
		.max_extent.width = me.width,
		.max_extent.height = me.height,
	};
	return true;
}

static bool query_modifier_support(struct wlr_vk_device *dev,
		struct wlr_vk_format_props *props, size_t modifier_count) {
	VkDrmFormatModifierPropertiesListEXT modp = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		.drmFormatModifierCount = modifier_count,
	};
	VkFormatProperties2 fmtp = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modp,
	};

	// the first call to vkGetPhysicalDeviceFormatProperties2 did only
	// retrieve the number of modifiers, we now have to retrieve
	// the modifiers
	modp.pDrmFormatModifierProperties =
		calloc(modifier_count, sizeof(*modp.pDrmFormatModifierProperties));
	if (!modp.pDrmFormatModifierProperties) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return false;
	}

	vkGetPhysicalDeviceFormatProperties2(dev->phdev, props->format.vk, &fmtp);

	props->render_mods =
		calloc(modp.drmFormatModifierCount, sizeof(*props->render_mods));
	props->texture_mods =
		calloc(modp.drmFormatModifierCount, sizeof(*props->texture_mods));
	if (!props->render_mods || !props->texture_mods) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		free(modp.pDrmFormatModifierProperties);
		free(props->render_mods);
		free(props->texture_mods);
		return false;
	}

	bool found = false;
	for (uint32_t i = 0; i < modp.drmFormatModifierCount; ++i) {
		VkDrmFormatModifierPropertiesEXT m = modp.pDrmFormatModifierProperties[i];
		char render_status[256], texture_status[256];

		// check that specific modifier for render usage
		// also, only allow rendering to formats with SRGB encoding
		const char *errmsg = "unknown error";
		if ((m.drmFormatModifierTilingFeatures & render_features) == render_features &&
				props->format.is_srgb) {
			struct wlr_vk_format_modifier_props p = {0};
			if (query_modifier_usage_support(dev, props->format.vk, render_usage, &m, &p, &errmsg)) {
				props->render_mods[props->render_mod_count++] = p;
				wlr_drm_format_set_add(&dev->dmabuf_render_formats,
					props->format.drm, m.drmFormatModifier);
				found = true;
			}
		} else {
			errmsg = "missing required features";
		}
		if (errmsg != NULL) {
			snprintf(render_status, sizeof(render_status), "✗ render (%s)", errmsg);
		} else {
			snprintf(render_status, sizeof(render_status), "✓ render");
		}

		// check that specific modifier for texture usage
		errmsg = "unknown error";
		if ((m.drmFormatModifierTilingFeatures & dma_tex_features) == dma_tex_features) {
			struct wlr_vk_format_modifier_props p = {0};
			if (query_modifier_usage_support(dev, props->format.vk, dma_tex_usage, &m, &p, &errmsg)) {
				props->texture_mods[props->texture_mod_count++] = p;
				wlr_drm_format_set_add(&dev->dmabuf_texture_formats,
					props->format.drm, m.drmFormatModifier);
				found = true;
			}
		} else {
			errmsg = "missing required features";
		}
		if (errmsg != NULL) {
			snprintf(texture_status, sizeof(texture_status), "✗ texture (%s)", errmsg);
		} else {
			snprintf(texture_status, sizeof(texture_status), "✓ texture");
		}

		char *modifier_name = drmGetFormatModifierName(m.drmFormatModifier);
		wlr_log(WLR_DEBUG, "    DMA-BUF modifier %s "
			"(0x%016"PRIX64", %"PRIu32" planes): %s  %s",
			modifier_name ? modifier_name : "<unknown>", m.drmFormatModifier,
			m.drmFormatModifierPlaneCount, texture_status, render_status);
		free(modifier_name);
	}

	free(modp.pDrmFormatModifierProperties);
	return found;
}

void vulkan_format_props_query(struct wlr_vk_device *dev,
		const struct wlr_vk_format *format) {
	VkResult res;

	char *format_name = drmGetFormatName(format->drm);
	wlr_log(WLR_DEBUG, "  %s (0x%08"PRIX32")",
		format_name ? format_name : "<unknown>", format->drm);
	free(format_name);

	VkDrmFormatModifierPropertiesListEXT modp = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 fmtp = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modp,
	};

	vkGetPhysicalDeviceFormatProperties2(dev->phdev, format->vk, &fmtp);

	bool add_fmt_props = false;
	struct wlr_vk_format_props props = {0};
	props.format = *format;

	// non-dmabuf texture properties
	const char *shm_texture_status;
	if ((fmtp.formatProperties.optimalTilingFeatures & tex_features) == tex_features) {
		VkPhysicalDeviceImageFormatInfo2 fmti = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
			.type = VK_IMAGE_TYPE_2D,
			.format = format->vk,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = tex_usage,
		};
		VkImageFormatProperties2 ifmtp = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		};

		res = vkGetPhysicalDeviceImageFormatProperties2(dev->phdev, &fmti, &ifmtp);
		if (res != VK_SUCCESS) {
			if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
				shm_texture_status = "✗ texture (unsupported format)";
			} else {
				wlr_vk_error("vkGetPhysicalDeviceImageFormatProperties2", res);
				shm_texture_status = "✗ texture (failed to get format properties)";
			}
		} else {
			VkExtent3D me = ifmtp.imageFormatProperties.maxExtent;
			props.max_extent.width = me.width;
			props.max_extent.height = me.height;
			props.features = fmtp.formatProperties.optimalTilingFeatures;

			shm_texture_status = "✓ texture";

			dev->shm_formats[dev->shm_format_count] = format->drm;
			++dev->shm_format_count;

			add_fmt_props = true;
		}
	} else {
		shm_texture_status = "✗ texture (missing required features)";
	}
	wlr_log(WLR_DEBUG, "    Shared memory: %s", shm_texture_status);

	if (modp.drmFormatModifierCount > 0) {
		add_fmt_props |= query_modifier_support(dev, &props,
			modp.drmFormatModifierCount);
	}

	if (add_fmt_props) {
		dev->format_props[dev->format_prop_count] = props;
		++dev->format_prop_count;
	} else {
		vulkan_format_props_finish(&props);
	}
}

void vulkan_format_props_finish(struct wlr_vk_format_props *props) {
	free(props->texture_mods);
	free(props->render_mods);
}

const struct wlr_vk_format_modifier_props *vulkan_format_props_find_modifier(
		struct wlr_vk_format_props *props, uint64_t mod, bool render) {
	if (render) {
		for (unsigned i = 0u; i < props->render_mod_count; ++i) {
			if (props->render_mods[i].props.drmFormatModifier == mod) {
				return &props->render_mods[i];
			}
		}
	} else {
		for (unsigned i = 0u; i < props->texture_mod_count; ++i) {
			if (props->texture_mods[i].props.drmFormatModifier == mod) {
				return &props->texture_mods[i];
			}
		}
	}

	return NULL;
}
