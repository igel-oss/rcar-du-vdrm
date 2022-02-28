# What is this?

This driver code is added virtual DRM support to the car-du driver included in the linux-kernel of [Renesas Yocto 5.9.0](https://github.com/renesas-rcar/meta-renesas/tree/Renesas-Yocto-v5.9.0).

# Test environment and test procedures

## Environment

Use the rootfs built by the yocto recipe [Renesas Yocto 5.9.0](https://github.com/renesas-rcar/meta-renesas/tree/Renesas-Yocto-v5.9.0).

## Procedure

1. Build
    1. Build Linux kernel.
    2. Build this driver code.
2. Install kernel module to rootfs
3. Test
    1. Boot the R-Car board.
    2. Load this driver module.
    3. Run modetest.

# Build
## Build Linux kernel
The linux kernel created by [Renesas Yocto 5.9.0](https://github.com/renesas-rcar/meta-renesas/tree/Renesas-Yocto-v5.9.0) includes the rcar-du driver, so you need to change the '.config' and build it. In addition, the virtual DRM device needs to be added to the device tree, so the device tree needs to be rebuilt.

### Update .config

Update the .config as follows:
```diff
--- .config_for_5.10.41
+++ .config
-CONFIG_DRM_RCAR_DU=y
-CONFIG_DRM_RCAR_CMM=y
+CONFIG_DRM_RCAR_DU=n
+CONFIG_DRM_RCAR_CMM=n
 CONFIG_DRM_RCAR_DW_HDMI=y
-CONFIG_DRM_RCAR_LVDS=y
-CONFIG_DRM_RCAR_MIPI_DSI=y
-CONFIG_DRM_RCAR_VSP=y
-CONFIG_DRM_RCAR_WRITEBACK=y
+CONFIG_DRM_RCAR_LVDS=n
+CONFIG_DRM_RCAR_MIPI_DSI=n
+CONFIG_DRM_RCAR_VSP=n
+CONFIG_DRM_RCAR_WRITEBACK=n
```

### Update VSP1 driver

The patch [vsp1: commit the same Display List until a next vsync interrupt.](misc/patch/0001-vsp1-commit-the-same-Display-List-until-a-next-vsync.patch) is applied to linux kernel.
(This patch is workarround!)

### Update Device Tree

In order to use virtual DRM, a 'vdrm' node is created and specified to 'vdrms' property of the 'du' node.

#### **'vdrm' node**

Required properties:

- compatible: must be "virt-drm"

The 'vdrm' node must include at only one 'plane' child node.

#### **'plane' node**

Required properties:

- x: x-coordinate of the letf-top of the plane
- y: y-coordinate of the letf-top of the plane
- width: width of the plane
- height: height of the plane
- zpos: z-position of the plane

#### **Example**

```C
vdrm0: vdrm@0 {
    compatible = "virt-drm";
    plane@0 {
        x = <200>;
        y = <100>;
        width = <800>;
        height = <600>;
        zpos = <1>;
    };
};

&du {
    vdrms = <&vdrm0>;
};
```

In the dts/ directory, there are samples for H3ulcb and M3ulcb.

## build this driver code

Use vdrm-dev branch of this repo.

To build using Yocto SDK, set the environment variables for poky.
```bash
$ . /opt/poky/3.1.11/environment-setup-aarch64-poky-linux
```

The following environment variables should be unset because they contain flags that are not supported by the kernel build.

```bash
$ unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS MACHINE
```

Build this driver by specifying linux kernel path to KERNELSRC.
```bash
$ KERNELSRC=/path/to/kernel-src/ make
```

# Install kernel module to rootfs

Copy the following kernel module to /rootfs/lib/modules/5.10.41-yocto-standard/extra/.

* /path/to/rcar-du/rcar-du-drm.ko
* /path/to/rcar-du/rcar_cmm.ko
* /path/to/rcar-du/rcar_lvds.ko
* /path/to/rcar-du/rcar_mipi_dsi.ko

# Test
## Boot the R-Car board

Boot the board using the kernel image and DTB that is built.

Notice: Currently, virtual DRM only supports HDMI0, so please connect it to HDMI0.

## Load this driver module

```
# insmod /lib/modules/5.10.41-yocto-standard/extra/rcar_lvds.ko
# insmod /lib/modules/5.10.41-yocto-standard/extra/rcar_cmm.ko
# insmod /lib/modules/5.10.41-yocto-standard/extra/rcar_mipi_dsi.ko
# insmod /lib/modules/5.10.41-yocto-standard/extra/rcar-du-drm.ko
```

## Run modetest

The driver module name is 'virt-drm'. So the information of the virtual DRM device can be looked with the following command.
```
# modetest -M virt-drm
```

If there are multiple virtual DRM devices, the bus-id is the dtb node name (vdrm@0), so the command in modetest will be as follows.
```
# modetest -M virt-drm -D vdrm@0
```

To test in atomic mode, use the following command.
where, this is the case for connector ID=48, plane ID=38, and crtc ID=47, so you may need to change the numbers to suit your environment.
```
# modetest -M virt-drm -s 48:800x600 -P 38@47:800x600 -v -a
```
