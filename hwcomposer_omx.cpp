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
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <utils/Trace.h>

#include <hardware/hardware.h>

#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include <log/log.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>

#include <hardware/hwcomposer.h>

#include <IDisplayManager.h>
#include <EGL/egl.h>
#include <ui/GraphicBufferMapper.h>

#include <OMX_Core.h>
#include <OMX_Component.h>
#include <OMX_Index_SNI.h>
#include <OMX_IVCommon_SNI.h>
#include <OMX_Video_SNI.h>
// #include <OMX_Other_SNI.h>

#include "asm/vocd_driver.h"

#include <hardware/gralloc.h>
#include "mali_gralloc_module.h"
#include "gralloc_priv.h"

#include <GLES/gl.h>

enum {
    VSYNC_FALSE = 0,
    VSYNC_TRUE
};

extern "C" {
int clock_nanosleep(clockid_t clock_id, int flags,
                           const struct timespec *request,
                           struct timespec *remain);
}

/*****************************************************************************/
struct hwc_context_t {
    hwc_composer_device_1_t device;
    /* our private state goes below here */
    const hwc_procs_t       *procs;
    int32_t                 xres;
    int32_t                 yres;
    int32_t                 xdpi;
    int32_t                 ydpi;
    int32_t                 vsync_period;
    nsecs_t                 mNextFakeVSync;
    private_module_t  *gralloc_module;
    framebuffer_device_t          *alloc_device;
    pthread_t               vsync_thread;
    int32_t                 vsync_thread_f;
    buffer_handle_t        *pHandle;
    int32_t                 has_private0;
    int32_t                 current_Config;

    pthread_mutex_t         vsync_lock;
    bool                    vsync_callback_enabled;
};

sp<IDisplayManager>     DisplayManagerLocal;

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    .open = hwc_device_open,
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWC_MODULE_API_VERSION_0_1,
        .hal_api_version    = HARDWARE_HAL_API_VERSION,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "Sample hwcomposer module",
        .author = "The Android Open Source Project",
        .methods = &hwc_module_methods,
        .dso = NULL,
        .reserved = {0},
    }
};

static int32_t num_config = 0;
static dm_display_attribute mConfigs[16];

/*****************************************************************************/

static pthread_mutex_t g_lock;
static pthread_cond_t g_wait;
static int g_ev;

OMX_ERRORTYPE init_omx(void)
{
    ATRACE_CALL();
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    ALOGI("init_omx() in hwcomposer_omx.cpp START");
    while (1) {
        DisplayManagerLocal = getDemoServ();
        if (DisplayManagerLocal != NULL) {
            break;
        }
    }
    ALOGI("init_omx() in hwcomposer_omx.cpp END");
    return ( ret );
}

OMX_ERRORTYPE finish_omx(void)
{
    ATRACE_CALL();
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    return ( ret );
}

static void hwc_emptyThisBuffer(native_handle_t const*hnd)
{
    ATRACE_CALL();
    static native_handle_t *pre_hnd = NULL;
    if (pre_hnd != hnd) {
        DisplayManagerLocal->EmptyBuffer(IDisplayManager::TYPE_OSD, hnd);
        pre_hnd = (native_handle_t *)hnd;
    }
    return;
}


static int update_plane_show(int has_overlay)
{
    ATRACE_CALL();
    static int pre_has_overlay = 0;
    if (pre_has_overlay != has_overlay) {
        DisplayManagerLocal->ChangePlaneShow(has_overlay);
        pre_has_overlay = has_overlay;
    }
    return 0;
}


/*****************************************************************************/

#ifdef ENABLE_DUMP_LAYER
static void dump_layer(hwc_layer_1_t const* l) {
    ALOGD("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}",
            l->compositionType, l->flags, l->handle, l->transform, l->blending,
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom);
}
#endif /* ENABLE_DUMP_LAYER */


static int hwc_prepare_primary(hwc_composer_device_1_t *dev __unused,
        hwc_display_contents_1_t *fimd_contents)
{
    ATRACE_CALL();
    int has_overlay = 0;
    
    if (fimd_contents && (fimd_contents->flags & HWC_GEOMETRY_CHANGED)) {
        for (size_t i=0 ; i<fimd_contents->numHwLayers ; i++) {
            switch (fimd_contents->hwLayers[i].compositionType) {
            case HWC_FRAMEBUFFER_TARGET:
                // Don't care this layer.
                continue;
            case HWC_BACKGROUND:
                // Currently, we don't support this layer. so toggle to HWC_FRAMEBUFFER.
                ALOGI("HWC_BACKGROUND!!!!!!!!");
                fimd_contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
                has_overlay |= IDisplayManager::TYPE_OSD;
                continue;
            case HWC_SIDEBAND:
                // Currently, we don't support this layer. so toggle to HWC_FRAMEBUFFER.
                ALOGI("HWC_SIDEBAND!!!!!!!!");
                fimd_contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
                has_overlay |= IDisplayManager::TYPE_OSD;
                continue;
            default:
                break;
            }
            
            if(fimd_contents->hwLayers[i].handle == NULL){
                fimd_contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
                has_overlay |= IDisplayManager::TYPE_OSD;
                // ALOGE("handle is NULL (hwc_prepare)!!!!!!!!!!!!");
            }else{
                //Other
#ifdef ENABLE_DUMP_LAYER
                dump_layer(&fimd_contents->hwLayers[i]);
#endif /* ENABLE_DUMP_LAYER */
                fimd_contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
                has_overlay |= IDisplayManager::TYPE_OSD;
                // ALOGE("flag is Unknown (hwc_prepare)!!!!!!!!!!!!");
            }
        }
    }
    {
        if (fimd_contents && (fimd_contents->flags & HWC_GEOMETRY_CHANGED)) {
            // Update show status
            update_plane_show(has_overlay);
        }
    }
    return 0;
}

static int hwc_prepare(hwc_composer_device_1_t *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    ATRACE_CALL();
    int ret = 0;
    for (int32_t dpy = ((int32_t)numDisplays-1); dpy >=0 ; dpy--) {
        hwc_display_contents_1_t *fimd_contents = displays[dpy];
        switch(dpy) {
            case HWC_DISPLAY_PRIMARY:
                ret = hwc_prepare_primary(dev, fimd_contents);
                break;
            case HWC_DISPLAY_EXTERNAL:
            case HWC_DISPLAY_VIRTUAL:
            default:
                ret = -EINVAL;
                break;
        }
    }
    return ret;
}

void closeAcquireFds(hwc_display_contents_1_t* list)
{
    ATRACE_CALL();
    if (list != NULL) {
        for(uint32_t i = 0; i < list->numHwLayers; i++) {
            //Close the acquireFenceFds
            //HWC_FRAMEBUFFER are -1 already by SF, rest we close.
            if(list->hwLayers[i].acquireFenceFd >= 0) {
                close(list->hwLayers[i].acquireFenceFd);
                list->hwLayers[i].acquireFenceFd = -1;
            }
        }
        //Writeback
        if(list->outbufAcquireFenceFd >= 0) {
            close(list->outbufAcquireFenceFd);
            list->outbufAcquireFenceFd = -1;
        }
    }
}

static void hwc_glfinish(void) {
    ATRACE_CALL();
    glFinish();
}


static int hwc_set_primary(hwc_context_t *dev __unused,
        hwc_display_contents_1_t *fimd_contents)
{
    ATRACE_CALL();

    for (size_t i=0 ; i<fimd_contents->numHwLayers ; i++) {
#ifdef ENABLE_DUMP_LAYER
        dump_layer(&fimd_contents->hwLayers[i]);
#endif /* ENABLE_DUMP_LAYER */
        if (fimd_contents->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET) {
            if (fimd_contents->hwLayers[i].handle != NULL) {
                // ALOGI("set HWC_FRAMEBUFFER_TARGET!!!!!!!!");
                hwc_glfinish();
                hwc_emptyThisBuffer(fimd_contents->hwLayers[i].handle);
            }
        }
    }
    closeAcquireFds(fimd_contents);
    return 0;
}


static int hwc_set(hwc_composer_device_1 *dev,
                   size_t numDisplays,
                   hwc_display_contents_1_t** displays)
{
    ATRACE_CALL();
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    for (int32_t dpy = ((int32_t)numDisplays-1); dpy >=0 ; dpy--) {
        hwc_display_contents_1_t* list = displays[dpy];
        switch(dpy) {
            case HWC_DISPLAY_PRIMARY:
                ret = hwc_set_primary(ctx, list);
                break;
            case HWC_DISPLAY_EXTERNAL:
            case HWC_DISPLAY_VIRTUAL:
            default:
                closeAcquireFds(list);
                ret = -EINVAL;
                break;
        }
    }
    return ret;
}

static int hwc_setPowerMode(struct hwc_composer_device_1 *dev __unused, int disp, int mode)
{
    ATRACE_CALL();
    int result = 0;
    
    ALOGI("%s:%d is called. disp: %d mode %d", __func__, __LINE__, disp, mode);
    switch (disp) {
    case HWC_DISPLAY_PRIMARY:
        DisplayManagerLocal->SetPowerMode(mode);
        break;
    case HWC_DISPLAY_EXTERNAL:
    case HWC_DISPLAY_VIRTUAL:
    default:
        return -EINVAL;
    }
    return result;
}

static int hwc_query(struct hwc_composer_device_1* dev, int what, int *value)
{
    ATRACE_CALL();
    struct hwc_context_t *pdev =
            (struct hwc_context_t *)dev;
    ALOGI("%s:%d is called", __func__, __LINE__);
    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // we support the background layer
        value[0] = 0;
        break;
    case HWC_DISPLAY_TYPES_SUPPORTED:
        value[0] = HWC_DISPLAY_PRIMARY_BIT; // Only support the Primariy Display.
        break;
    case HWC_VSYNC_PERIOD:
        // vsync period in nanosecond
        value[0] = pdev->vsync_period;
        break;
    default:
        // unsupported query
        return -EINVAL;
    }
    return 0;
}

static int hwc_eventControl(struct hwc_composer_device_1 *dev, int dpy __unused,
        int event, int enabled)
{
    ATRACE_CALL();
    struct hwc_context_t *pdev =
            (struct hwc_context_t *)dev;

    ALOGI("%s:%d is called : %d = %d", __func__, __LINE__, event, enabled);
    switch (event) {
    case HWC_EVENT_VSYNC:
        if (!(enabled & ~1)) {
            pthread_mutex_lock(&pdev->vsync_lock);
            pdev->vsync_callback_enabled = enabled;
            pthread_mutex_unlock(&pdev->vsync_lock);
            return 0;
        }
        break;
    default:
        break;
    }

    return -EINVAL;
}

static void hwc_registerProcs(struct hwc_composer_device_1* dev,
        hwc_procs_t const* procs)
{
    ATRACE_CALL();
    struct hwc_context_t* pdev =
            (struct hwc_context_t*)dev;
    // ALOGI("%s:%d is called", __func__, __LINE__);
    pdev->procs = procs;
}

static void hwc_dump(hwc_composer_device_1* dev __unused, char *buff, int buff_len)
{
    ATRACE_CALL();
    // ALOGI("%s:%d is called", __func__, __LINE__);
    String8 dump = DisplayManagerLocal->Dump();
    int copy_len = std::min((int)(buff_len-1), (int)dump.size());
    strncpy(buff, dump.c_str(), copy_len);
    buff[copy_len] = '\0';
}


static int hwc_getDisplayConfigs(struct hwc_composer_device_1 *dev __unused,
        int disp, uint32_t *configs, size_t *numConfigs)
{
    ATRACE_CALL();

    if (*numConfigs == 0)
        return 0;

    if (disp == HWC_DISPLAY_PRIMARY) {
        if (num_config != 0) {
            for (int i=0; i<num_config; i++) {
                configs[i] = i;
            }
            *numConfigs = num_config;
            return 0;
        }
    }
    return -EINVAL;
}


static int32_t hwc_fimd_attribute(struct hwc_context_t *pdev,
        uint32_t config, const uint32_t attribute)
{
    ATRACE_CALL();
    switch(attribute) {
    case HWC_DISPLAY_VSYNC_PERIOD:
        return 1000000000LL / mConfigs[config].fps;

    case HWC_DISPLAY_WIDTH:
        return mConfigs[config].losd_xres;

    case HWC_DISPLAY_HEIGHT:
        return mConfigs[config].losd_yres;

    case HWC_DISPLAY_DPI_X:
        return pdev->xdpi;

    case HWC_DISPLAY_DPI_Y:
        return pdev->ydpi;

    case HWC_DISPLAY_COLOR_TRANSFORM:
        return 0;

    default:
        ALOGE("unknown display attribute %u", attribute);
        return -EINVAL;
    }
}


static int hwc_getDisplayAttributes(struct hwc_composer_device_1 *dev,
        int disp, uint32_t config, const uint32_t *attributes, int32_t *values)
{
    ATRACE_CALL();
    struct hwc_context_t *pdev =
                   (struct hwc_context_t *)dev;

    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
        if (disp == HWC_DISPLAY_PRIMARY)
            values[i] = hwc_fimd_attribute(pdev, config, attributes[i]);
        else {
            ALOGE("unknown display type %u", disp);
            return -EINVAL;
        }
    }

    return 0;
}

static int hwc_getActiveConfig(struct hwc_composer_device_1* dev, int disp)
{
    ATRACE_CALL();
    struct hwc_context_t *pdev =
                    (struct hwc_context_t *)dev;

    if (disp == HWC_DISPLAY_PRIMARY) {
        return pdev->current_Config;
    }
    return -EINVAL;
}


static int hwc_setActiveConfig(struct hwc_composer_device_1* dev, int disp, int index)
{
    ATRACE_CALL();
    struct hwc_context_t *pdev =
                    (struct hwc_context_t *)dev;
    if (disp == HWC_DISPLAY_PRIMARY) {
        if ((0 <= index) && (index < num_config)) {
            if (pdev->current_Config != index) {
//                ALOGI("hwc_setActiveConfig : set to %d", index);
                int ret = DisplayManagerLocal->setActiveConfig(index);
                if (ret < 0) {
                    ALOGE("%s:%d error : %d", __func__, __LINE__, ret);
                    return -EINVAL;
                }
                pdev->current_Config = index;
            }
            return 0;
        } else {
            ALOGE("%s:%d error : %d num_config %d", __func__, __LINE__, index, num_config);
            return -EINVAL;
        }
    }
    return -EINVAL;
}


static void *hwc_vsync_thread(void *data)
{
    struct hwc_context_t *pdev =
            (struct hwc_context_t *)data;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);
    pdev->mNextFakeVSync = pdev->vsync_period;
    bool vsync_enabled = false;

    int fd_bldd;
    VOCD_IOC_ARG arg;
    int IOC_CMD = VOCD_IOC_WAIT_VSYNC5;
    /* open Blender driver */
    fd_bldd = open( "/dev/vocd", O_RDWR | O_SYNC );
    if( fd_bldd < 0 ){
        ALOGE( "[hwcomposer] Error: failed to open fd_bldd = %d\n", fd_bldd );
    }


    while (pdev->vsync_thread_f == VSYNC_TRUE) {
        ioctl( fd_bldd, IOC_CMD, &arg );
        pthread_mutex_lock(&pdev->vsync_lock);
        vsync_enabled = pdev->vsync_callback_enabled;
        pthread_mutex_unlock(&pdev->vsync_lock);

        uint64_t timestamp = (uint64_t)systemTime(CLOCK_MONOTONIC);
        if ((pdev->procs != NULL) && (pdev->procs->vsync != NULL)) {
            if (vsync_enabled) {
                pdev->procs->vsync(pdev->procs, 0, timestamp);
            }
        }
    }

    close( fd_bldd );
    return 0;
}

static int hwc_device_close(struct hw_device_t *dev)
{
    ATRACE_CALL();
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;

    finish_omx();

    if (ctx) {
        if (ctx->vsync_thread) {
            void *retval;
            ctx->vsync_thread_f = VSYNC_FALSE;
            pthread_join(ctx->vsync_thread, &retval);
        }
        free(ctx);
    }
    return 0;
}

/*****************************************************************************/

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    ATRACE_CALL();
    int status = -EINVAL;
    struct hwc_context_t *dev;

    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
        dev = (hwc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                (const struct hw_module_t **)&dev->gralloc_module)) {
            ALOGE("failed to get gralloc hw module");
            status = -EINVAL;
            goto err_get_module;
        }

        if (framebuffer_open((const hw_module_t *)dev->gralloc_module,
                &dev->alloc_device)) {
            ALOGE("failed to open framebuffer");
            status = -EINVAL;
            goto err_get_module;
        }

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = hwc_device_close;

        dev->device.prepare = hwc_prepare;
        dev->device.set = hwc_set;
        dev->device.eventControl = hwc_eventControl;
        dev->device.common.version = HWC_DEVICE_API_VERSION_1_5;
        dev->device.setPowerMode = hwc_setPowerMode;
        dev->device.getActiveConfig = hwc_getActiveConfig;
        dev->device.setActiveConfig = hwc_setActiveConfig;
        dev->device.query = hwc_query;
        dev->device.registerProcs = hwc_registerProcs;
        dev->device.dump = hwc_dump;
        dev->device.getDisplayConfigs = hwc_getDisplayConfigs;
        dev->device.getDisplayAttributes = hwc_getDisplayAttributes;

        dev->xdpi = dev->gralloc_module->xdpi*1000;
        dev->ydpi = dev->gralloc_module->ydpi*1000;
        dev->has_private0 = 0;
        dev->current_Config = 0;

//        /* for calling OMX_EmptyThisBuffer() from fb_post() in gralloc */
//        dev->gralloc_module->hwc_emptyThisBuffer = hwc_emptyThisBuffer;

        pthread_mutex_init( &g_lock, NULL );
        pthread_cond_init( &g_wait, NULL );
        g_ev = 0;

        init_omx();

        {
            int ret = -1;
            // Get Display Config
            dm_display_attribute *ptr = &mConfigs[0];
            ret = DisplayManagerLocal->GetDisplayConfigs(&num_config, &ptr);
            while (ret != NO_ERROR) {
                struct timespec spec;
                uint64_t timestamp = (uint64_t)systemTime(CLOCK_MONOTONIC);
                timestamp += 100*1000*1000; // 100msec
                spec.tv_sec  = timestamp / 1000000000;
                spec.tv_nsec = timestamp % 1000000000;
                do {
                    ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &spec, NULL);
                } while (ret<0 && errno == EINTR);
                ret = DisplayManagerLocal->GetDisplayConfigs(&num_config, &ptr);
            }

            dev->xres = mConfigs[0].losd_xres;
            dev->yres = mConfigs[0].losd_yres;
            dev->vsync_period = 1000000000LL / mConfigs[0].fps;
        }

        *device = &dev->device.common;
        status = 0;

        pthread_mutex_init(&dev->vsync_lock, NULL);
        dev->vsync_thread_f = VSYNC_TRUE;
        status = pthread_create(&dev->vsync_thread, NULL, hwc_vsync_thread, dev);
        if (status) {
            ALOGE("failed to start vsync thread: %s", strerror(status));
            status = -status;
            goto err_vsync;
        }
    }
    return status;
err_vsync:
#ifdef USE_GRALLOC_API
    gralloc_close(dev->alloc_device);
#else
    framebuffer_close(dev->alloc_device);
#endif
err_get_module:
    free(dev);
    return status;
}

