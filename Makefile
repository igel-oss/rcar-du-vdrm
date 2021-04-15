# SPDX-License-Identifier: GPL-2.0

# These are used as a branching condition in DU driver code,
# and if it is not defined, a build error occurs.
ccflags-y := -DCONFIG_DRM_RCAR_VSP -DCONFIG_DRM_RCAR_LVDS

rcar-du-drm-y := rcar_du_crtc.o \
		 rcar_du_drv.o \
		 rcar_du_encoder.o \
		 rcar_du_group.o \
		 rcar_du_kms.o \
		 rcar_du_plane.o \
		 rcar_du_vdrm.o \
		 virtual/vdrm_drv.o

# LVDS
rcar-du-drm-y	+= rcar_du_of.o \
		   rcar_du_of_lvds_r8a7790.dtb.o \
		   rcar_du_of_lvds_r8a7791.dtb.o \
		   rcar_du_of_lvds_r8a7793.dtb.o \
		   rcar_du_of_lvds_r8a7795.dtb.o \
		   rcar_du_of_lvds_r8a7796.dtb.o
# VSP
rcar-du-drm-y	+= rcar_du_vsp.o
rcar-du-drm-y	+= rcar_du_writeback.o

obj-m += rcar-du-drm.o
obj-m += rcar_lvds.o

# 'remote-endpoint' is fixed up at run-time
DTC_FLAGS_rcar_du_of_lvds_r8a7790 += -Wno-graph_endpoint
DTC_FLAGS_rcar_du_of_lvds_r8a7791 += -Wno-graph_endpoint
DTC_FLAGS_rcar_du_of_lvds_r8a7793 += -Wno-graph_endpoint
DTC_FLAGS_rcar_du_of_lvds_r8a7795 += -Wno-graph_endpoint
DTC_FLAGS_rcar_du_of_lvds_r8a7796 += -Wno-graph_endpoint

all:
	make -C $(KERNELSRC) M=$(PWD) modules
clean:
	make -C $(KERNELSRC) M=$(PWD) clean
