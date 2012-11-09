/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <hardware/hardware.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hwcomposer.h>
#include "../gralloc/gralloc_priv.h"
#include <linux/xylonbb.h>

#include <EGL/egl.h>

/*****************************************************************************/

struct hwc_context_t {
    hwc_composer_device_t device;
    /* our private state goes below here */
    int fd;
    int bb_fd;
};

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: HWC_HARDWARE_MODULE_ID,
        name: "Sample hwcomposer module",
        author: "The Android Open Source Project",
        methods: &hwc_module_methods,
    }
};

/*****************************************************************************/

static void dump_layer(int layer_number, hwc_layer_t const* l) {
    const private_handle_t *phnd = static_cast<const private_handle_t *>(l->handle);
    ALOGD("%d \ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d} stride=%d",
            layer_number,
            l->compositionType, l->flags, l->handle, l->transform, l->blending,
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom,
          phnd ? phnd->stride : -1);
}

static int hwc_prepare(hwc_composer_device_t *dev, hwc_layer_list_t* list) {
    if (list->numHwLayers > 1) {
        ALOGD("hwc_prepare");
        if (list && (list->flags & HWC_GEOMETRY_CHANGED)) {
            for (size_t i=0 ; i<list->numHwLayers ; i++) {
                dump_layer(i, &list->hwLayers[i]);
                if (i > 0)
                    list->hwLayers[i].compositionType = HWC_OVERLAY;
                hwc_layer_t const* l = &list->hwLayers[i];
                if (((l->displayFrame.right - l->displayFrame.left)
                     != (l->sourceCrop.right - l->sourceCrop.left))
                    || ((l->displayFrame.top - l->displayFrame.bottom)
                        != (l->sourceCrop.top - l->sourceCrop.bottom)))
                    ALOGD("needs scaling");
                if (l->handle) {
                    const private_handle_t *phnd = static_cast<const private_handle_t *>(l->handle);
                    private_handle_t::validate(phnd);
                }
            }
        }
    }
    return 0;
}

static void bitblitLayer(hwc_context_t *context, hwc_layer_t *l0, hwc_layer_t *l)
{
    const private_handle_t *surfaceHandle =
        static_cast<const private_handle_t *>(l0->handle);
    if (!surfaceHandle) {
        ALOGD("null base layer");
        return;
    }

    unsigned long *surfaceBase = (unsigned long*)surfaceHandle->base;
    size_t surfaceStride = surfaceHandle->stride;
    int surfaceLeft = l0->sourceCrop.left;
    int surfaceTop = l0->sourceCrop.top;

    int displayLeft = l->displayFrame.left;
    int displayTop = l->displayFrame.top;
    if (!l->handle)
        return;
    const private_handle_t *layerHandle =
        static_cast<const private_handle_t *>(l->handle);
    unsigned long *layerBase = (unsigned long *)layerHandle->base;
    size_t layerStride = layerHandle->stride;

    int layerLeft = l->sourceCrop.left;
    int layerTop = l->sourceCrop.top;
    int columns = l->sourceCrop.right - l->sourceCrop.left;
    int rows = l->sourceCrop.bottom - l->sourceCrop.top;

    ALOGD("bitblitLayer: surfaceBase=%p layerBase=%p sLeft=%d sTop=%d dLeft=%d dTop=%d lLeft=%d lTop=%d",
          surfaceBase, layerBase, surfaceLeft, surfaceTop, displayLeft, displayTop, layerLeft, layerTop);

    if (context->bb_fd) {
        struct xylonbb_params params;
        ALOGD("surfaceHandle=%d layerHandle=%d", surfaceHandle->fd, layerHandle->fd);
        params.dst_dma_buf = surfaceHandle->fd;
        params.dst_offset = 4*(displayLeft + surfaceLeft + (displayTop + surfaceTop)*surfaceStride);
        params.dst_stripe = surfaceStride;
        params.src_dma_buf = layerHandle->fd;
        params.src_offset = 4*(layerLeft + layerTop*layerStride);
        params.src_stripe = layerStride;
        params.num_columns = columns;
        params.num_rows = rows;
        int status = ioctl(context->bb_fd, XYLONBB_IOC_BITBLIT, &params);
        if (status == 0)
            return;
    }

    for (int i = 0; i < columns; i++) {
        for (int j = 0; j < rows; j++) {
            if ((displayLeft + surfaceLeft + i 
                 + (j+displayTop+surfaceTop)*surfaceStride)*4 > surfaceHandle->size) {
                ALOGD("base ref out of bounds");
                return;
            }
            if ((layerLeft + i + (j+layerTop)*layerStride)*4 > layerHandle->size) {
                ALOGD("layer ref out of bounds: layerLeft %d i %d j %d layerTop %d layerStride %d size %d",
                      layerLeft, i, j, layerTop, layerStride, layerHandle->size);
                return;
            }
            surfaceBase[displayLeft + surfaceLeft + i + (j+displayTop+surfaceTop)*surfaceStride] =
                layerBase[layerLeft + i + (j+layerTop)*layerStride];
        }
    }
}

static int hwc_set(hwc_composer_device_t *dev,
        hwc_display_t dpy,
        hwc_surface_t sur,
        hwc_layer_list_t* list)
{
    if (list->numHwLayers > 1) {
        ALOGD("hwc_set dpy=%p surface=%p\n", dpy, sur);

        hwc_layer_t *l0 = &list->hwLayers[0];
        hwc_context_t *context = (struct hwc_context_t *)dev;
        
        for (size_t i=0 ; i<list->numHwLayers ; i++) {
            dump_layer(i, &list->hwLayers[i]);
            if (i > 0) {
                bitblitLayer(context, l0, &list->hwLayers[i]);
            }
        }
    }

    EGLBoolean success = eglSwapBuffers((EGLDisplay)dpy, (EGLSurface)sur);
    if (!success) {
        return HWC_EGL_ERROR;
    }
    return 0;
}

static int hwc_device_close(struct hw_device_t *dev)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    if (ctx) {
        free(ctx);
    }
    return 0;
}

/*****************************************************************************/

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
        struct hwc_context_t *dev;
        dev = (hwc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = hwc_device_close;

        dev->device.prepare = hwc_prepare;
        dev->device.set = hwc_set;

        *device = &dev->device.common;

        int fd = open("/dev/xylonbb", O_RDWR);
        if (fd)
            dev->bb_fd = fd;
        if (fd <= 0) {
            ALOGE("failed to open /dev/xylonbb: fd=%d errno=%d:%s\n",
                  fd, errno, strerror(errno));
        }
        status = 0;
    }
    return status;
}
