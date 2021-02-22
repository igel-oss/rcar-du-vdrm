/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * vdrm_api.h -- Virtual DRM API
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 */

#ifndef __VDRM_API__
#define __VDRM_API__

#include <linux/of_device.h>
#include <drm/drm_crtc.h>

struct vdrm_property_info {
	struct drm_property *prop;
	uint64_t default_val;
};

struct vdrm_display {
	struct drm_crtc *crtc;
	struct drm_crtc *parent_crtc;
};

/* callback */
struct vdrm_funcs {
	int (*dumb_create)(struct drm_file *file, struct drm_device *dev,
			   struct drm_mode_create_dumb *args);
	void (*crtc_flush)(struct vdrm_display *vdisplay);
	const struct drm_plane_funcs *plane;
	const struct drm_plane_helper_funcs *plane_helper;
};

void vdrm_drv_handle_vblank(struct vdrm_display *vdisplay);
void vdrm_drv_finish_page_flip(struct vdrm_display *vdisplay);
struct drm_device *vdrm_drv_init(struct drm_device *dev,
				 int num_props,
				 struct vdrm_property_info *props,
				 const struct vdrm_funcs *funcs);
int vdrm_drv_display_init(struct drm_device *dev,
			  struct vdrm_display *vdisplay,
			  struct device_node *np,
			  struct drm_crtc *crtc, struct drm_plane *plane,
			  int num_formats, const u32 *formats,
			  int max_zpos);
int vdrm_drv_register(struct drm_device *dev, const char *name);
void vdrm_drv_fini(struct drm_device *dev);

#endif /* __VDRM_API__ */
