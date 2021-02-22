// SPDX-License-Identifier: GPL-2.0+
/*
 * vdrm_drv.c -- Virtual DRM driver
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 *
 * This driver is based on drivers/gpu/drm/drm_simple_kms_helper.c.
 */

#include <linux/of_device.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <video/videomode.h>

#include "vdrm_drv.h"

static inline struct vdrm_drv_display *
to_vdrm_drv_display(struct drm_connector *connector)
{
	return container_of(connector, struct vdrm_drv_display, connector);
}

static inline struct vdrm_drv_display *
crtc_to_vdrm_drv_display(struct drm_crtc *crtc)
{
	return container_of(crtc, struct vdrm_drv_display, crtc);
}

static int vdrm_dumb_create(struct drm_file *file, struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	struct vdrm_device *vdrm = dev->dev_private;

	return vdrm->funcs->dumb_create(file, dev, args);
}

struct vdrm_framebuffer {
	struct drm_framebuffer fb;
	struct drm_framebuffer *parent_fb;
};

static inline struct vdrm_framebuffer *
to_vdrm_framebuffer(struct drm_framebuffer *fb)
{
	return container_of(fb, struct vdrm_framebuffer, fb);
}

static void vdrm_fb_destroy(struct drm_framebuffer *fb)
{
	struct vdrm_framebuffer *vfb = to_vdrm_framebuffer(fb);

	vfb->parent_fb->funcs->destroy(vfb->parent_fb);
	drm_framebuffer_cleanup(fb);
	kfree(vfb);
}

static const struct drm_framebuffer_funcs vdrm_fb_funcs = {
	.destroy = vdrm_fb_destroy,
};

static int vdrm_fb_init(struct drm_device *dev, struct vdrm_framebuffer *vfb)
{
	vfb->fb = *vfb->parent_fb;
	vfb->fb.dev = dev;

	return drm_framebuffer_init(dev, &vfb->fb, &vdrm_fb_funcs);
}

static struct drm_framebuffer *
vdrm_fb_create(struct drm_device *dev, struct drm_file *file_priv,
	       const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct vdrm_device *vdrm = dev->dev_private;
	const struct drm_mode_config_funcs *mode_config_funcs =
		vdrm->parent->mode_config.funcs;
	struct vdrm_framebuffer *vfb;
	struct drm_framebuffer *fb;
	int ret;

	vfb = kzalloc(sizeof(*vfb), GFP_KERNEL);
	if (!vfb)
		return ERR_PTR(-ENOMEM);

	fb = mode_config_funcs->fb_create(vdrm->parent, file_priv, mode_cmd);
	if (IS_ERR(fb)) {
		kfree(vfb);
		return fb;
	}

	vfb->parent_fb = fb;
	ret = vdrm_fb_init(dev, vfb);
	if (ret) {
		fb->funcs->destroy(fb);
		kfree(vfb);
		return ERR_PTR(ret);
	}

	return &vfb->fb;
}

static const struct drm_mode_config_funcs vdrm_mode_config_funcs = {
	.fb_create = vdrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static struct drm_display_mode *vdrm_create_mode(struct vdrm_drv_display *disp)
{
	struct drm_display_mode *mode;
	struct videomode videomode;

	mode = drm_mode_create(disp->dev->ddev);
	if (!mode)
		return NULL;

	memset(&videomode, 0, sizeof(videomode));
	videomode.hactive = disp->plane_info.width;
	videomode.vactive = disp->plane_info.height;
	videomode.pixelclock =
		disp->pipe->parent_crtc->state->adjusted_mode.crtc_clock * 1000;
	mode->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	drm_display_mode_from_videomode(&videomode, mode);

	return mode;
}

static int vdrm_connector_get_mode(struct drm_connector *connector)
{
	struct vdrm_drv_display *disp = to_vdrm_drv_display(connector);
	struct drm_display_mode *mode = vdrm_create_mode(disp);

	if (!mode)
		return 0;

	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_connector_helper_funcs vdrm_conn_helper_funcs = {
	.get_modes = vdrm_connector_get_mode,
};

static const struct drm_connector_funcs vdrm_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void vdrm_drv_finish_page_flip_internal(struct vdrm_drv_display *disp)
{
	struct drm_device *dev = disp->dev->ddev;
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	event = disp->event;
	disp->event = NULL;
	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (event == NULL)
		return;

	spin_lock_irqsave(&dev->event_lock, flags);
	drm_crtc_send_vblank_event(&disp->crtc, event);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (disp->vblank_count) {
		drm_crtc_vblank_put(&disp->crtc);
		disp->vblank_count--;
	}
}

static void vdrm_plane_update(struct drm_plane *plane,
			      struct drm_plane_state *old_state)
{
	struct vdrm_device *vdrm = plane->dev->dev_private;
	struct drm_crtc *vcrtc_old_state = old_state->crtc;
	struct drm_crtc *vcrtc_plane_state = plane->state->crtc;
	struct drm_crtc *crtc;
	struct vdrm_drv_display *vdisplay;

	crtc = (old_state->crtc ? old_state->crtc : plane->state->crtc);
	if (WARN_ON(!crtc))
		return;

	vdisplay = crtc_to_vdrm_drv_display(crtc);

	old_state->crtc = vdisplay->pipe->parent_crtc;
	plane->state->crtc = vdisplay->pipe->parent_crtc;

	plane->state->dst.x1 += vdisplay->plane_info.x;
	plane->state->dst.y1 += vdisplay->plane_info.y;
	vdrm->funcs->plane_helper->atomic_update(plane, old_state);

	old_state->crtc = vcrtc_old_state;
	plane->state->crtc = vcrtc_plane_state;
}

static void vdrm_plane_reset(struct drm_plane *plane)
{
	struct vdrm_device *vdrm = plane->dev->dev_private;
	struct vdrm_drv_display *disp;

	vdrm->funcs->plane->reset(plane);

	list_for_each_entry(disp, &vdrm->disps, head) {
		if (disp->plane == plane)
			break;
	}
	if (WARN_ON(!disp))
		return;

	plane->state->zpos = disp->plane_info.z;
}

static struct drm_property *
vdrm_find_parent_property(struct vdrm_device *vdrm, struct drm_property *prop)
{
	int i;

	for (i = 0; i < vdrm->num_props; i++) {
		if (vdrm->props[i].prop == prop)
			return vdrm->props[i].parent_prop;
	}

	return NULL;
}

static int vdrm_plane_set_property(struct drm_plane *plane,
				   struct drm_plane_state *state,
				   struct drm_property *property,
				   uint64_t val)
{
	struct vdrm_device *vdrm = plane->dev->dev_private;
	struct vdrm_drv_display *disp;
	struct drm_property *parent_prop;

	parent_prop = vdrm_find_parent_property(vdrm, property);
	if (parent_prop && vdrm->funcs->plane->atomic_set_property)
		return vdrm->funcs->plane->atomic_set_property(plane, state,
							       parent_prop,
							       val);

	list_for_each_entry(disp, &vdrm->disps, head) {
		if (disp->plane == plane)
			break;
	}
	if (WARN_ON(!disp))
		return -EINVAL;

	if (vdrm->plane_props.offset_x == property) {
		if (val > disp->pipe->parent_crtc->mode.hdisplay)
			return -EINVAL;
		disp->plane_info.x = val;
	} else if (vdrm->plane_props.offset_y == property) {
		if (val > disp->pipe->parent_crtc->mode.vdisplay)
			return -EINVAL;
		disp->plane_info.y = val;
	} else if (vdrm->plane_props.width == property) {
		if (val > disp->pipe->parent_crtc->mode.hdisplay)
			return -EINVAL;
		disp->plane_info.width = val;
	} else if (vdrm->plane_props.height == property) {
		if (val > disp->pipe->parent_crtc->mode.vdisplay)
			return -EINVAL;
		disp->plane_info.height = val;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int vdrm_plane_get_property(struct drm_plane *plane,
				   const struct drm_plane_state *state,
				   struct drm_property *property,
				   uint64_t *val)
{
	struct vdrm_device *vdrm = plane->dev->dev_private;
	struct vdrm_drv_display *disp;
	struct drm_property *parent_prop;

	parent_prop = vdrm_find_parent_property(vdrm, property);
	if (parent_prop && vdrm->funcs->plane->atomic_get_property)
		return vdrm->funcs->plane->atomic_get_property(plane, state,
							       parent_prop,
							       val);

	list_for_each_entry(disp, &vdrm->disps, head) {
		if (disp->plane == plane)
			break;
	}
	if (WARN_ON(!disp))
		return -EINVAL;

	if (vdrm->plane_props.offset_x == property)
		*val = disp->plane_info.x;
	else if (vdrm->plane_props.offset_y == property)
		*val = disp->plane_info.y;
	else if (vdrm->plane_props.width == property)
		*val = disp->plane_info.width;
	else if (vdrm->plane_props.height == property)
		*val = disp->plane_info.height;
	else
		return -EINVAL;

	return 0;
}

static int vdrm_crtc_check(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	bool has_primary = state->plane_mask &
				drm_plane_mask(crtc->primary);

	/* We always want to have an active plane with an active CRTC */
	if (has_primary != state->enable)
		return -EINVAL;

	return drm_atomic_add_affected_planes(state->state, crtc);
}

static void vdrm_crtc_flush(struct drm_crtc *crtc,
			    struct drm_crtc_state *old_state)
{
	struct vdrm_drv_display *disp = crtc_to_vdrm_drv_display(crtc);
	struct vdrm_device *vdrm = disp->dev;

	if (crtc->state->event) {
		struct drm_device *dev = crtc->dev;
		unsigned long flags;

		if (disp->crtc_enabled) {
			WARN_ON(drm_crtc_vblank_get(crtc) != 0);
			disp->vblank_count++;
		}

		spin_lock_irqsave(&dev->event_lock, flags);
		disp->event = crtc->state->event;
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}

	if (vdrm->funcs->crtc_flush)
		vdrm->funcs->crtc_flush(disp->pipe);
}

static void vdrm_crtc_enable(struct drm_crtc *crtc,
			     struct drm_crtc_state *old_state)
{
	struct vdrm_drv_display *disp = crtc_to_vdrm_drv_display(crtc);

	drm_crtc_vblank_on(crtc);
	disp->crtc_enabled = true;
}

static void vdrm_crtc_disable(struct drm_crtc *crtc,
			      struct drm_crtc_state *old_state)
{
	struct vdrm_drv_display *disp = crtc_to_vdrm_drv_display(crtc);
	unsigned long flags;
	bool pending;

	disp->crtc_enabled = false;
	drm_crtc_vblank_off(crtc);

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	pending = disp->event != NULL;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

	if (!wait_event_timeout(disp->flip_wait, !pending,
				msecs_to_jiffies(50))) {
		DRM_WARN("VDRM: page flip timeout\n");
		vdrm_drv_finish_page_flip_internal(disp);
	}

	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static const struct drm_crtc_helper_funcs vdrm_crtc_helper_funcs = {
	.atomic_check = vdrm_crtc_check,
	.atomic_flush = vdrm_crtc_flush,
	.atomic_enable = vdrm_crtc_enable,
	.atomic_disable = vdrm_crtc_disable,
};

static int vdrm_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct vdrm_drv_display *disp;

	disp = crtc_to_vdrm_drv_display(crtc);
	disp->vblank_enabled = true;

	return 0;
}

static void vdrm_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct vdrm_drv_display *disp;

	disp = crtc_to_vdrm_drv_display(crtc);
	disp->vblank_enabled = false;
}

static const struct drm_crtc_funcs vdrm_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = vdrm_crtc_enable_vblank,
	.disable_vblank = vdrm_crtc_disable_vblank,
};
static const struct drm_encoder_funcs vdrm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int
vdrm_connector_init(struct vdrm_device *vdrm,
		    struct vdrm_drv_display *disp,
		    const u32 *formats, unsigned int num_formats)
{
	int ret;

	disp->dev = vdrm;
	ret = drm_connector_init(vdrm->ddev, &disp->connector,
				 &vdrm_conn_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret < 0)
		return ret;

	drm_connector_helper_add(&disp->connector,
				 &vdrm_conn_helper_funcs);

	drm_plane_helper_add(disp->plane, &vdrm->drm_plane_helper_funcs);
	ret = drm_universal_plane_init(vdrm->ddev, disp->plane, 0,
				       &vdrm->drm_plane_funcs,
				       formats, num_formats,
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(&disp->crtc, &vdrm_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(vdrm->ddev, &disp->crtc,
					disp->plane, NULL,
					&vdrm_crtc_funcs, NULL);
	disp->encoder.possible_crtcs =
		drm_crtc_mask(&disp->crtc);
	ret = drm_encoder_init(vdrm->ddev, &disp->encoder,
			       &vdrm_encoder_funcs, DRM_MODE_ENCODER_NONE,
			       NULL);
	if (ret)
		return ret;

	return drm_connector_attach_encoder(&disp->connector,
					    &disp->encoder);
}

static int vdrm_properties_init(struct vdrm_device *vdrm, int num_props,
				struct vdrm_property_info *props)
{
	int i;
	unsigned int w = vdrm->ddev->mode_config.max_width;
	unsigned int h = vdrm->ddev->mode_config.max_height;

	vdrm->plane_props.offset_x =
		drm_property_create_range(vdrm->ddev, 0, "vdrm_offset_x", 0, w);
	if (!vdrm->plane_props.offset_x)
		return -1;
	vdrm->plane_props.offset_y =
		drm_property_create_range(vdrm->ddev, 0, "vdrm_offset_y", 0, h);
	if (!vdrm->plane_props.offset_y)
		return -1;
	vdrm->plane_props.width =
		drm_property_create_range(vdrm->ddev, 0, "vdrm_width", 1, w);
	if (!vdrm->plane_props.width)
		return -1;
	vdrm->plane_props.height =
		drm_property_create_range(vdrm->ddev, 0, "vdrm_height", 1, h);
	if (!vdrm->plane_props.height)
		return -1;

	if (num_props == 0)
		return 0;

	vdrm->props = devm_kzalloc(vdrm->parent->dev,
				    sizeof(*vdrm->props) * num_props,
				    GFP_KERNEL);
	if (!vdrm->props)
		return -ENOMEM;

	for (i = 0; i < num_props; i++) {
		struct drm_property *p = props[i].prop;

		vdrm->props[i].prop =
			drm_property_create_range(vdrm->ddev, p->flags,
						  p->name, p->values[0],
						  p->values[1]);
		if (!vdrm->props[i].prop)
			goto err;

		vdrm->props[i].parent_prop = p;
		vdrm->props[i].default_val = props[i].default_val;
	}
	vdrm->num_props = num_props;

	return 0;

err:
	for (i--; i >= 0; i--)
		drm_property_destroy(vdrm->ddev, vdrm->props[i].prop);
	devm_kfree(vdrm->parent->dev, vdrm->props);
	return -1;
}

static int vdrm_of_get_plane(struct device_node *np,
			     int *x, int *y, int *width, int *height, int *z)
{
	struct device_node *child;
	int ret;

	child = of_get_next_child(np, NULL);
	if (!child)
		return -ENODEV;

	ret = of_property_read_u32(child, "x", x);
	ret |= of_property_read_u32(child, "y", y);
	ret |= of_property_read_u32(child, "width", width);
	ret |= of_property_read_u32(child, "height", height);
	ret |= of_property_read_u32(child, "zpos", z);

	of_node_put(child);
	return ret;
}

static void vdrm_dump(struct vdrm_device *vdrm)
{
	struct vdrm_drv_display *disp;

	DRM_INFO("Virtual DRM Info:\n");
	list_for_each_entry(disp, &vdrm->disps, head) {
		DRM_INFO("\tCONNECTOR: %d\n",
			 disp->connector.base.id);
		DRM_INFO("\tCRTC: %d\n",
			 disp->crtc.base.id);
		DRM_INFO("\tENCODER: %d\n",
			 disp->encoder.base.id);
		DRM_INFO("\tPLANE: %d\n",
			 disp->plane->base.id);
		DRM_INFO("\tParent CRTC: %d\n",
			 disp->pipe->parent_crtc->base.id);
	}
}

void vdrm_drv_handle_vblank(struct vdrm_display *vdisplay)
{
	struct vdrm_drv_display *disp =
		crtc_to_vdrm_drv_display(vdisplay->crtc);

	if (disp->vblank_enabled)
		drm_crtc_handle_vblank(vdisplay->crtc);
}

void vdrm_drv_finish_page_flip(struct vdrm_display *vdisplay)
{
	struct vdrm_drv_display *disp =
		crtc_to_vdrm_drv_display(vdisplay->crtc);

	vdrm_drv_finish_page_flip_internal(disp);
}

DEFINE_DRM_GEM_CMA_FOPS(vdrm_fops);

static struct drm_driver vdrm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.gem_vm_ops = &drm_gem_cma_vm_ops,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_get_sg_table = drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.dumb_create = vdrm_dumb_create,
	.gem_prime_vmap = drm_gem_cma_prime_vmap,
	.gem_prime_vunmap = drm_gem_cma_prime_vunmap,
	.fops = &vdrm_fops,
	.name = "virt-drm",
	.desc = "Virtual DRM driver",
	.date = "20201104",
	.major = 1,
	.minor = 0,
};

struct drm_device *vdrm_drv_init(struct drm_device *dev,
				 int num_props,
				 struct vdrm_property_info *props,
				 const struct vdrm_funcs *funcs)
{
	struct vdrm_device *vdrm;
	struct drm_device *ddev;
	int ret;

	vdrm = kzalloc(sizeof(*vdrm), GFP_KERNEL);
	if (vdrm == NULL)
		return NULL;

	vdrm->parent = dev;
	vdrm->funcs = funcs;

	vdrm->drm_plane_funcs = *funcs->plane;
	vdrm->drm_plane_funcs.reset = vdrm_plane_reset;
	vdrm->drm_plane_funcs.atomic_set_property = vdrm_plane_set_property;
	vdrm->drm_plane_funcs.atomic_get_property = vdrm_plane_get_property;
	vdrm->drm_plane_helper_funcs = *funcs->plane_helper;
	vdrm->drm_plane_helper_funcs.atomic_update = vdrm_plane_update;

	ddev = drm_dev_alloc(&vdrm_driver, dev->dev);
	if (IS_ERR(ddev))
		goto failed;

	vdrm->ddev = ddev;
	ddev->dev_private = vdrm;

	INIT_LIST_HEAD(&vdrm->disps);

	drm_mode_config_init(ddev);

	ddev->mode_config.min_width = 0;
	ddev->mode_config.min_height = 0;
	ddev->mode_config.max_width = 8190;
	ddev->mode_config.max_height = 8190;
	ddev->mode_config.normalize_zpos = true;
	ddev->mode_config.funcs = &vdrm_mode_config_funcs;

	ret = vdrm_properties_init(vdrm, num_props, props);
	if (ret < 0)
		goto failed;

	return ddev;

failed:
	kfree(vdrm);
	return NULL;
}

int vdrm_drv_display_init(struct drm_device *dev,
			  struct vdrm_display *vdisplay,
			  struct device_node *np,
			  struct drm_crtc *crtc, struct drm_plane *plane,
			  int num_formats, const u32 *formats,
			  int max_zpos)
{
	struct vdrm_device *vdrm = dev->dev_private;
	struct vdrm_drv_display *disp;
	struct vdrm_plane_info plane_info;
	int i, ret;

	if (!of_device_is_compatible(np, "virt-drm"))
		return -ENODEV;

	ret = vdrm_of_get_plane(np, &plane_info.x, &plane_info.y,
				&plane_info.width, &plane_info.height,
				&plane_info.z);
	if (ret < 0) {
		DRM_WARN("VDRM: failed get plane node of %s\n",
			 of_node_full_name(np));
		return ret;
	}

	disp = kzalloc(sizeof(*disp), GFP_KERNEL);
	if (!disp)
		return -ENOMEM;

	disp->plane = plane;
	disp->plane_info = plane_info;
	ret = vdrm_connector_init(vdrm, disp, formats, num_formats);
	if (ret < 0) {
		DRM_WARN("VDRM: Failed connector initialization.\n");
		return ret;
	}

	drm_plane_create_zpos_property(plane,
				       disp->plane_info.z, 0, max_zpos);
	drm_object_attach_property(&plane->base,
				   vdrm->plane_props.offset_x,
				   disp->plane_info.x);
	drm_object_attach_property(&plane->base,
				   vdrm->plane_props.offset_y,
				   disp->plane_info.y);
	drm_object_attach_property(&plane->base,
				   vdrm->plane_props.width,
				   disp->plane_info.width);
	drm_object_attach_property(&plane->base,
				   vdrm->plane_props.height,
				   disp->plane_info.height);
	for (i = 0; i < vdrm->num_props; i++) {
		drm_object_attach_property(&plane->base,
					   vdrm->props[i].prop,
					   vdrm->props[i].default_val);
	}

	init_waitqueue_head(&disp->flip_wait);

	INIT_LIST_HEAD(&disp->head);
	list_add_tail(&disp->head, &vdrm->disps);
	vdrm->num_crtcs++;

	vdisplay->crtc = &disp->crtc;
	vdisplay->parent_crtc = crtc;
	disp->pipe = vdisplay;

	return 0;
}

int vdrm_drv_register(struct drm_device *dev, const char *name)
{
	int ret;
	struct vdrm_device *vdrm = dev->dev_private;

	ret = drm_vblank_init(dev, vdrm->num_crtcs);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_dev_set_unique(dev, name);
	dev->irq_enabled = true;

	DRM_INFO("Virtual Device is initialized.\n");

	vdrm_dump(dev->dev_private);

	return 0;
}

void vdrm_drv_fini(struct drm_device *dev)
{
	struct vdrm_device *vdrm = dev->dev_private;
	struct vdrm_drv_display *disp;

	if (dev) {
		if (dev->registered)
			drm_dev_unregister(dev);
		drm_mode_config_cleanup(dev);
		drm_dev_put(dev);
	}
	list_for_each_entry(disp, &vdrm->disps, head)
		kfree(disp);
	kfree(vdrm);
}
