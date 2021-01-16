#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <drm_fourcc.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/log.h>
#include "render/allocator/vulkan.h"

#if defined(__linux__)
#include <sys/sysmacros.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#else
#error "Missing major/minor for this platform"
#endif

static const struct wlr_buffer_impl buffer_impl;

static struct wlr_vulkan_buffer *vulkan_buffer_from_buffer(
		struct wlr_buffer *wlr_buf) {
	assert(wlr_buf->impl == &buffer_impl);
	return (struct wlr_vulkan_buffer *)wlr_buf;
}

static bool buffer_get_dmabuf(struct wlr_buffer *wlr_buf,
		struct wlr_dmabuf_attributes *out) {
	struct wlr_vulkan_buffer *buf = vulkan_buffer_from_buffer(wlr_buf);
	memcpy(out, &buf->dmabuf, sizeof(buf->dmabuf));
	return true;
}

static void buffer_destroy(struct wlr_buffer *wlr_buf) {
	struct wlr_vulkan_buffer *buf = vulkan_buffer_from_buffer(wlr_buf);
	wlr_dmabuf_attributes_finish(&buf->dmabuf);
	vkFreeMemory(buf->alloc->device, buf->memory, NULL);
	vkDestroyImage(buf->alloc->device, buf->image, NULL);
	free(buf);
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};

static const struct wlr_allocator_interface allocator_impl;

static struct wlr_vulkan_allocator *vulkan_alloc_from_alloc(
		struct wlr_allocator *wlr_alloc) {
	assert(wlr_alloc->impl == &allocator_impl);
	return (struct wlr_vulkan_allocator *)wlr_alloc;
}

static const struct wlr_vulkan_allocator_modifier *find_mod(struct wlr_vulkan_allocator *alloc,
		uint64_t mod) {
	for (size_t i = 0; i < alloc->mods_len; i++) {
		const struct wlr_vulkan_allocator_modifier *mod_info = &alloc->mods[i];
		if (mod_info->props.drmFormatModifier == mod) {
			return mod_info;
		}
	}
	return NULL;
}

static struct wlr_buffer *allocator_create_buffer(
		struct wlr_allocator *wlr_alloc, int width, int height,
		const struct wlr_drm_format *drm_format) {
	struct wlr_vulkan_allocator *alloc = vulkan_alloc_from_alloc(wlr_alloc);

	// TODO: do not hardcode
	assert(drm_format->format == DRM_FORMAT_ARGB8888 ||
		drm_format->format == DRM_FORMAT_XRGB8888);
	VkFormat vk_format = VK_FORMAT_B8G8R8A8_SRGB;

	uint64_t *mods = calloc(drm_format->len, sizeof(mods[0]));
	if (mods == NULL) {
		return NULL;
	}

	size_t mods_len = 0;
	for (size_t i = 0; i < drm_format->len; i++) {
		const struct wlr_vulkan_allocator_modifier *mod =
			find_mod(alloc, drm_format->modifiers[i]);
		if (mod == NULL) {
			continue;
		}

		if (mod->max_extent.width < (uint32_t)width ||
				mod->max_extent.height < (uint32_t)height) {
			continue;
		}

		// TODO: add support for DISJOINT images?

		mods[mods_len++] = mod->props.drmFormatModifier;
	}

	if (mods_len == 0) {
		wlr_log(WLR_ERROR, "Found zero compatible format modifiers");
		return NULL;
	}

	struct wlr_vulkan_buffer *buf = calloc(1, sizeof(*buf));
	if (buf == NULL) {
		return NULL;
	}
	wlr_buffer_init(&buf->base, &buffer_impl, width, height);
	buf->alloc = alloc;

	VkImageDrmFormatModifierListCreateInfoEXT drm_format_mod = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
		.drmFormatModifierCount = mods_len,
		.pDrmFormatModifiers = mods,
	};
	VkExternalMemoryImageCreateInfo ext_mem = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &drm_format_mod,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo img_create = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &ext_mem,
		.imageType = VK_IMAGE_TYPE_2D,
		.extent = { .width = width, .height = height, .depth = 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.format = vk_format,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.samples = VK_SAMPLE_COUNT_1_BIT,
	};

	VkResult res = vkCreateImage(alloc->device, &img_create, NULL, &buf->image);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkCreateImage failed");
		goto error_buf;
	}

	free(mods);

	VkMemoryRequirements mem_reqs = {0};
	vkGetImageMemoryRequirements(alloc->device, buf->image, &mem_reqs);

	VkExportMemoryAllocateInfo export_mem = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo mem_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &export_mem,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = 0, // TODO
	};

	res = vkAllocateMemory(alloc->device, &mem_alloc, NULL, &buf->memory);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkAllocateMemory failed");
		goto error_image;
	}

	res = vkBindImageMemory(alloc->device, buf->image, buf->memory, 0);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkBindImageMemory failed");
		goto error_memory;
	}

	PFN_vkGetImageDrmFormatModifierPropertiesEXT vkGetImageDrmFormatModifierPropertiesEXT =
		(PFN_vkGetImageDrmFormatModifierPropertiesEXT)
		vkGetInstanceProcAddr(alloc->instance, "vkGetImageDrmFormatModifierPropertiesEXT");
	assert(vkGetImageDrmFormatModifierPropertiesEXT != NULL);
	PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)
		vkGetInstanceProcAddr(alloc->instance, "vkGetMemoryFdKHR");
	assert(vkGetMemoryFdKHR != NULL);

	VkImageDrmFormatModifierPropertiesEXT img_mod_props = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
	};
	res = vkGetImageDrmFormatModifierPropertiesEXT(alloc->device, buf->image, &img_mod_props);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkGetImageDrmFormatModifierPropertiesEXT failed");
		goto error_memory;
	}

	VkMemoryGetFdInfoKHR mem_get_fd = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory = buf->memory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	int fd = -1;
	res = vkGetMemoryFdKHR(alloc->device, &mem_get_fd, &fd);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkGetMemoryFdKHR failed");
		goto error_memory;
	}

	const struct wlr_vulkan_allocator_modifier *mod =
		find_mod(alloc, img_mod_props.drmFormatModifier);
	assert(mod != NULL);
	assert(mod->props.drmFormatModifierPlaneCount <= WLR_DMABUF_MAX_PLANES);

	buf->dmabuf = (struct wlr_dmabuf_attributes){
		.format = drm_format->format,
		.modifier = img_mod_props.drmFormatModifier,
		.width = width,
		.height = height,
		.n_planes = mod->props.drmFormatModifierPlaneCount,
	};

	// Duplicate the first FD to all other planes
	buf->dmabuf.fd[0] = fd;
	for (uint32_t i = 1; i < mod->props.drmFormatModifierPlaneCount; i++) {
		int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
		if (dup_fd < 0) {
			wlr_log_errno(WLR_ERROR, "fcntl(F_DUPFD_CLOEXEC) failed");
			goto error_memory;
		}
		buf->dmabuf.fd[i] = dup_fd;
	}

	const VkImageAspectFlagBits plane_aspects[] = {
		VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
	};
	assert(mod->props.drmFormatModifierPlaneCount <=
		sizeof(plane_aspects) / sizeof(plane_aspects[0]));

	for (uint32_t i = 0; i < mod->props.drmFormatModifierPlaneCount; i++) {
		VkImageSubresource img_subres = {
			.aspectMask = plane_aspects[i],
		};
		VkSubresourceLayout subres_layout = {0};
		vkGetImageSubresourceLayout(alloc->device, buf->image, &img_subres, &subres_layout);

		buf->dmabuf.offset[i] = subres_layout.offset;
		buf->dmabuf.stride[i] = subres_layout.rowPitch;
	}

	return &buf->base;

error_memory:
	vkFreeMemory(buf->alloc->device, buf->memory, NULL);
error_image:
	vkDestroyImage(buf->alloc->device, buf->image, NULL);
error_buf:
	free(buf);
	return NULL;
}

static void allocator_destroy(struct wlr_allocator *wlr_alloc) {
	struct wlr_vulkan_allocator *alloc = vulkan_alloc_from_alloc(wlr_alloc);
	free(alloc->mods);
	vkDestroyDevice(alloc->device, NULL);
	vkDestroyInstance(alloc->instance, NULL);
	free(alloc);
}

static const struct wlr_allocator_interface allocator_impl = {
	.create_buffer = allocator_create_buffer,
	.destroy = allocator_destroy,
};

static VkPhysicalDevice find_phy_device_from_drm_fd(VkInstance instance,
		int drm_fd) {
	struct stat drm_stat;
	if (fstat(drm_fd, &drm_stat) != 0) {
		wlr_log_errno(WLR_ERROR, "fstat failed");
		return VK_NULL_HANDLE;
	}

	int64_t maj = (int64_t)major(drm_stat.st_rdev);
	int64_t min = (int64_t)minor(drm_stat.st_rdev);

	uint32_t devices_len = 0;
	vkEnumeratePhysicalDevices(instance, &devices_len, NULL);
	if (devices_len == 0) {
		wlr_log(WLR_ERROR, "No physical device found");
		return VK_NULL_HANDLE;
	}

	VkPhysicalDevice *devices = calloc(devices_len, sizeof(VkPhysicalDevice));
	if (devices == NULL) {
		return VK_NULL_HANDLE;
	}

	VkResult res = vkEnumeratePhysicalDevices(instance, &devices_len, devices);
	if (res != VK_SUCCESS) {
		free(devices);
		wlr_log(WLR_ERROR, "vkEnumeratePhysicalDevices failed");
		return VK_NULL_HANDLE;
	}

	VkPhysicalDevice dev = VK_NULL_HANDLE;
	for (size_t i = 0; i < devices_len; i++) {
		VkPhysicalDeviceDrmPropertiesEXT drm_props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &drm_props,
		};
		vkGetPhysicalDeviceProperties2(devices[i], &props);

		if (props.properties.apiVersion < VK_API_VERSION_1_1) {
			continue;
		}

		if (drm_props.hasPrimary && drm_props.primaryMajor == maj &&
				drm_props.primaryMinor == min) {
			dev = devices[i];
		}
		if (drm_props.hasRender && drm_props.renderMajor == maj &&
				drm_props.renderMinor == min) {
			dev = devices[i];
		}

		if (dev != VK_NULL_HANDLE) {
			wlr_log(WLR_DEBUG, "Physical device: %s",
				props.properties.deviceName);
			break;
		}
	}

	free(devices);

	return dev;
}

static bool create_device(struct wlr_vulkan_allocator *alloc) {
	const char *exts[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	};
	VkDeviceCreateInfo device_create = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]),
		.ppEnabledExtensionNames = exts,
	};
	VkResult res =
		vkCreateDevice(alloc->phy_device, &device_create, NULL, &alloc->device);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkCreateDevice failed");
		return false;
	}
	return true;
}

struct wlr_allocator *wlr_vulkan_allocator_create(int drm_fd) {
	struct wlr_vulkan_allocator *alloc = calloc(1, sizeof(*alloc));
	if (alloc == NULL) {
		return NULL;
	}
	wlr_allocator_init(&alloc->base, &allocator_impl, WLR_BUFFER_CAP_DMABUF);

	VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "wlroots",
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo instance_create = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app_info,
	};
	VkResult res = vkCreateInstance(&instance_create, NULL, &alloc->instance);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkCreateInstance failed");
		return NULL;
	}

	alloc->phy_device = find_phy_device_from_drm_fd(alloc->instance, drm_fd);
	if (alloc->phy_device == VK_NULL_HANDLE) {
		wlr_log(WLR_ERROR, "Failed to find physical device from DRM FD");
		return NULL;
	}

	if (!create_device(alloc)) {
		return NULL;
	}

	VkFormat vk_format = VK_FORMAT_B8G8R8A8_SRGB;
	VkDrmFormatModifierPropertiesListEXT mod_props_list = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 fmt_props = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &mod_props_list,
	};

	vkGetPhysicalDeviceFormatProperties2(alloc->phy_device, vk_format, &fmt_props);

	mod_props_list.pDrmFormatModifierProperties =
		calloc(mod_props_list.drmFormatModifierCount,
		sizeof(mod_props_list.pDrmFormatModifierProperties[0]));
	if (mod_props_list.pDrmFormatModifierProperties == NULL) {
		return NULL;
	}

	vkGetPhysicalDeviceFormatProperties2(alloc->phy_device, vk_format, &fmt_props);

	alloc->mods = calloc(mod_props_list.drmFormatModifierCount, sizeof(alloc->mods[0]));
	if (alloc->mods == NULL) {
		return NULL;
	}

	for (uint32_t i = 0; i < mod_props_list.drmFormatModifierCount; i++) {
		VkDrmFormatModifierPropertiesEXT props = mod_props_list.pDrmFormatModifierProperties[i];

		if (!(props.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
			continue;
		}

		VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
			.drmFormatModifier = props.drmFormatModifier,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VkPhysicalDeviceExternalImageFormatInfo ext_img_fmt_info = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
			.pNext = &mod_info,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
		VkPhysicalDeviceImageFormatInfo2 img_fmt_info = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
			.pNext = &ext_img_fmt_info,
			.type = VK_IMAGE_TYPE_2D,
			.format = vk_format,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		};

		VkExternalImageFormatProperties ext_img_fmt_props = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
		};
		VkImageFormatProperties2 img_fmt_props = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
			.pNext = &ext_img_fmt_props,
		};

		res = vkGetPhysicalDeviceImageFormatProperties2(alloc->phy_device,
			&img_fmt_info, &img_fmt_props);
		if (res != VK_SUCCESS) {
			if (res != VK_ERROR_FORMAT_NOT_SUPPORTED) {
				wlr_log(WLR_ERROR, "vkGetPhysicalDeviceImageFormatProperties2 failed");
			}
			continue;
		}

		VkExternalMemoryFeatureFlags ext_mem_features =
			ext_img_fmt_props.externalMemoryProperties.externalMemoryFeatures;
		if (!(ext_mem_features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
			continue;
		}

		VkExtent3D max_extent = img_fmt_props.imageFormatProperties.maxExtent;
		alloc->mods[alloc->mods_len++] = (struct wlr_vulkan_allocator_modifier){
			.props = props,
			.max_extent = { .width = max_extent.width, .height = max_extent.height },
		};
	}

	free(mod_props_list.pDrmFormatModifierProperties);

	if (alloc->mods_len == 0) {
		wlr_log(WLR_ERROR, "Found zero supported format modifiers");
		return NULL;
	}

	return &alloc->base;
}
