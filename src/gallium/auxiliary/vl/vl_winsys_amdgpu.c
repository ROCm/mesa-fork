/**************************************************************************
 *
 * Copyright 2015 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <assert.h>
#include <xf86drm.h>

#include "pipe/p_screen.h"
#include "pipe-loader/pipe_loader.h"
#include "util/os_file.h"
#include "util/driconf.h"
#include "util/u_memory.h"
#include "vl/vl_winsys.h"

#include "si_public.h"

struct pipe_screen_config;

struct pipe_loader_drm_device {
   struct pipe_loader_device base;
   const struct drm_driver_descriptor *dd;
   int fd;
};

#define pipe_loader_drm_device(dev) ((struct pipe_loader_drm_device *)dev)

static bool
drm_get_pci_id_for_fd(int fd, int *vendor_id, int *chip_id)
{
   drmDevicePtr device;

   if (drmGetDevice2(fd, 0, &device) != 0) {
      return false;
   }

   if (device->bustype != DRM_BUS_PCI) {
      drmFreeDevice(&device);
      return false;
   }

   *vendor_id = device->deviceinfo.pci->vendor_id;
   *chip_id = device->deviceinfo.pci->device_id;
   drmFreeDevice(&device);
   return true;
}

bool
pipe_loader_drm_probe_fd(struct pipe_loader_device **dev, int fd, bool zink)
{
   int new_fd;

   if (fd < 0 || (new_fd = os_dupfd_cloexec(fd)) < 0)
     return false;

   struct pipe_loader_drm_device *ddev = CALLOC_STRUCT(pipe_loader_drm_device);
   int vendor_id, chip_id;

   if (!ddev)
      return false;

   if (drm_get_pci_id_for_fd(new_fd, &vendor_id, &chip_id)) {
      ddev->base.u.pci.vendor_id = vendor_id;
      ddev->base.u.pci.chip_id = chip_id;
   } else {
      goto fail;
   }
   ddev->fd = new_fd;

   ddev->base.driver_name = strdup("radeonsi");

   *dev = &ddev->base;
   return true;

  fail:
   FREE(ddev->base.driver_name);
   FREE(ddev);

   close(new_fd);
   return false;
}

static void
pipe_loader_drm_release(struct pipe_loader_device **dev)
{
   struct pipe_loader_drm_device *ddev = pipe_loader_drm_device(*dev);

   close(ddev->fd);
   FREE(ddev->base.driver_name);
   FREE(*dev);
   *dev = 0;
}

static struct pipe_screen *
pipe_loader_drm_create_screen(struct pipe_loader_device *dev,
                              const struct pipe_screen_config *config, bool sw_vk)
{
   struct pipe_loader_drm_device *ddev = pipe_loader_drm_device(dev);

   return radeonsi_screen_create(ddev->fd, config);
}

static void
vl_drm_screen_destroy(struct vl_screen *vscreen);

struct vl_screen *
vl_drm_screen_create(int fd, bool honor_dri_prime)
{
   struct vl_screen *vscreen;
   int libva_owned_fd = -1;

   vscreen = CALLOC_STRUCT(vl_screen);
   if (!vscreen)
      return NULL;

   if (pipe_loader_drm_probe_fd(&vscreen->dev, fd, false))
   {
      struct pipe_screen_config config = {0};
      const driOptionDescription radeonsi_driconf[] = {
         #include "driinfo_radeonsi.h"
      };
      config.options = CALLOC_STRUCT(driOptionCache);
      config.options_info = CALLOC_STRUCT(driOptionCache);
      driParseOptionInfo((driOptionCache *)config.options_info, radeonsi_driconf, 
         ARRAY_SIZE(radeonsi_driconf));
      vscreen->pscreen = pipe_loader_drm_create_screen(vscreen->dev, &config, false);
   }

   if (libva_owned_fd >= 0 && libva_owned_fd != fd)
      close(fd);

   if (!vscreen->pscreen)
      goto release_pipe;

   vscreen->destroy = vl_drm_screen_destroy;
   vscreen->texture_from_drawable = NULL;
   vscreen->get_dirty_area = NULL;
   vscreen->get_timestamp = NULL;
   vscreen->set_next_timestamp = NULL;
   vscreen->get_private = NULL;
   return vscreen;

release_pipe:
   if (vscreen->dev)
      pipe_loader_drm_release(&vscreen->dev);

   FREE(vscreen);
   return NULL;
}

static void
vl_drm_screen_destroy(struct vl_screen *vscreen)
{
   assert(vscreen);

   vscreen->pscreen->destroy(vscreen->pscreen);
   pipe_loader_drm_release(&vscreen->dev);

   /* CHECK: The VAAPI loader/user preserves ownership of the original fd */
   FREE(vscreen);
}
