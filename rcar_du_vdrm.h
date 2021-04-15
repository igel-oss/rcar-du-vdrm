/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * rcar_du_vdrm.h -- R-Car Display Unit Virtual DRMs
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 */

#ifndef __RCAR_DU_VDRM_H__
#define __RCAR_DU_VDRM_H__

#include <drm/drm_atomic.h>

#include "rcar_du_drv.h"
#include "virtual/vdrm_api.h"

struct rcar_du_vdrm {
	struct drm_device *dev;
	struct vdrm_display vdrm_display[RCAR_DU_MAX_CRTCS];
};

int rcar_du_vdrms_init(struct rcar_du_device *rcdu);
void rcar_du_vdrms_fini(struct rcar_du_device *rcdu);
void rcar_du_vdrm_crtc_complete(struct rcar_du_crtc *crtc, unsigned int status);
void rcar_du_vdrm_vblank_event(struct rcar_du_crtc *crtc);
int rcar_du_vdrm_count(struct rcar_du_device *rcdu);

#endif /* __RCAR_DU_VDRM_H__ */
