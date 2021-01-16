#ifndef RENDER_ALLOCATOR_VULKAN_H
#define RENDER_ALLOCATOR_VULKAN_H

#include <vulkan/vulkan.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>

struct wlr_vulkan_buffer {
	struct wlr_buffer base;
	struct wlr_vulkan_allocator *alloc;

	VkImage image;
	VkDeviceMemory memory;

	struct wlr_dmabuf_attributes dmabuf;
};

struct wlr_vulkan_allocator {
	struct wlr_allocator base;

	VkInstance instance;
	VkPhysicalDevice phy_device;

	size_t mods_len;
	struct wlr_vulkan_allocator_modifier *mods;

	VkDevice device;
};

struct wlr_vulkan_allocator_modifier {
	VkDrmFormatModifierPropertiesEXT props;
	VkExtent2D max_extent;
};

struct wlr_allocator *wlr_vulkan_allocator_create(int drm_fd);

#endif
