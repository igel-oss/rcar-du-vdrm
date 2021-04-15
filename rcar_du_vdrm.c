// SPDX-License-Identifier: GPL-2.0+
/*
 * rcar_du_vdrm.c -- R-Car Display Unit Virtual DRMs
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 */

#include <linux/of_device.h>

#include <drm/drm_print.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <media/vsp1.h>

#include "rcar_du_vdrm.h"
#include "rcar_du_kms.h"
#include "rcar_du_crtc.h"
#include "rcar_du_vsp.h"
#include "virtual/vdrm_api.h"

extern const struct drm_plane_helper_funcs rcar_du_vsp_plane_helper_funcs;
extern const struct drm_plane_funcs rcar_du_vsp_plane_funcs;

static int rcar_du_vdrm_dumb_create(struct drm_file *file,
				    struct drm_device *dev,
				    struct drm_mode_create_dumb *args)
{
	/*
	 * TODO:
	 *   This is Warkarround.
	 *   In the future, this function will be removed.
	 *   The vdrm will be modified to directly call the dumb_create
	 *   callback of the du driver.
	 */
	unsigned int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	unsigned int align;

	/*
	 * The R8A7779 DU requires a 16 pixels pitch alignment as documented.
	 */
	align = 16 * args->bpp / 8;

	args->pitch = roundup(min_pitch, align);

	return drm_gem_cma_dumb_create_internal(file, dev, args);
}

static void rcar_du_vdrm_crtc_flush(struct vdrm_display *vdisplay)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(vdisplay->parent_crtc);

	rcar_du_vsp_atomic_flush(rcrtc);
}

static struct vdrm_funcs vdrm_funcs = {
	.dumb_create = rcar_du_vdrm_dumb_create,
	.crtc_flush = rcar_du_vdrm_crtc_flush,
};

void rcar_du_vdrm_crtc_complete(struct rcar_du_crtc *crtc, unsigned int status)
{
	struct rcar_du_vdrm_display *disp;

	list_for_each_entry(disp, &crtc->vdrm_displays, head) {
		vdrm_drv_handle_vblank(disp->display);
		if (status & VSP1_DU_STATUS_COMPLETE)
			vdrm_drv_finish_page_flip(disp->display);
	}
}

int rcar_du_vdrm_count(struct rcar_du_device *rcdu)
{
	const struct device_node *np = rcdu->dev->of_node;
	int num;

	num = of_property_count_u32_elems(np, "vdrms");
	if (num < 0)
		return 0;

	return num;
}

static int rcar_du_vdrm_init(struct rcar_du_device *rcdu,
			     struct rcar_du_vdrm *vdrm, struct device_node *np,
			     const u32 *formats, int num_formats)
{
	int i;
	struct vdrm_property_info props[] = {
		{
			.prop = rcdu->props.alpha,
			.default_val = 255,
		}, {
			.prop = rcdu->props.colorkey,
			.default_val = RCAR_DU_COLORKEY_NONE,
		}, {
			.prop = rcdu->props.colorkey_alpha,
			.default_val = 0,
		},
	};
	int num_vdrms = rcar_du_vdrm_count(rcdu);

	vdrm->dev = vdrm_drv_init(rcdu->ddev, ARRAY_SIZE(props), props,
				  &vdrm_funcs);
	if (!vdrm->dev)
		return -1;

	for (i = 0; i < RCAR_DU_MAX_CRTCS; i++) {
		struct rcar_du_crtc *rcrtc = &rcdu->crtcs[i];
		int plane_index = rcrtc->vsp->num_planes + rcdu->num_vdrms;
		struct rcar_du_vsp_plane *rplane =
			&rcrtc->vsp->planes[plane_index];
		int max_zpos = rcrtc->vsp->num_planes + num_vdrms;
		int ret;

		/*
		 * TODO:
		 *  Pass only the connected CRTCs to vDRM because the
		 *  vDRM driver desn't support hotplug.
		 *  In the future, it is necessary that hotplug is supported.
		 */
		if (!rcrtc->initialized)
			continue;

		ret = vdrm_drv_display_init(vdrm->dev, &vdrm->vdrm_display[i],
					    np, &rcrtc->crtc, &rplane->plane,
					    num_formats, formats, max_zpos);
		if (ret)
			return ret;

		rcar_du_crtc_add_vdrm_display(rcrtc, &vdrm->vdrm_display[i]);
		rplane->vdisplay = &vdrm->vdrm_display[i];
	}
	rcdu->num_vdrms++;

	return vdrm_drv_register(vdrm->dev, of_node_full_name(np));
}

int rcar_du_vdrms_init(struct rcar_du_device *rcdu)
{
	struct rcar_du_vdrm *vdrms;
	const u32 *formats;
	int num_formats, num_vdrms;
	int i, ret;

	num_vdrms = rcar_du_vdrm_count(rcdu);
	if (num_vdrms == 0)
		return 0;

	vdrms = kcalloc(num_vdrms, sizeof(*vdrms), GFP_KERNEL);
	if (!vdrms)
		return -1;

	DRM_INFO("VDRM: num vdrm = %d\n", num_vdrms);

	vdrm_funcs.plane = &rcar_du_vsp_plane_funcs;
	vdrm_funcs.plane_helper = &rcar_du_vsp_plane_helper_funcs;

	formats = rcar_du_get_plane_formats(&num_formats);
	for (i = 0; i < num_vdrms; i++) {
		struct of_phandle_args args;
		const struct device_node *np = rcdu->dev->of_node;

		ret = of_parse_phandle_with_fixed_args(np, "vdrms", 0, i,
						       &args);
		if (ret < 0) {
			DRM_WARN("VDRM: failed get vdrm%d.\n", i);
			goto err;
		}

		ret = rcar_du_vdrm_init(rcdu, &vdrms[i], args.np,
					formats, num_formats);
		of_node_put(args.np);
		if (ret < 0)
			goto err;
	}

	rcdu->vdrms = vdrms;
	return 0;

err:
	rcar_du_vdrms_fini(rcdu);
	rcdu->num_vdrms = 0;
	return ret;
}

void rcar_du_vdrms_fini(struct rcar_du_device *rcdu)
{
	int i;

	for (i = 0; i < rcdu->num_vdrms; i++) {
		if (rcdu->vdrms[i].dev)
			vdrm_drv_fini(rcdu->vdrms[i].dev);
	}

	for (i = 0; i < RCAR_DU_MAX_CRTCS; i++) {
		if (rcdu->crtcs[i].initialized)
			rcar_du_crtc_remove_vdrm_displays(&rcdu->crtcs[i]);
	}

	kfree(rcdu->vdrms);
}
