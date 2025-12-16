vlwinsys_deps = []
files_libgalliumvlwinsys = files('vl_winsys.h')
links_libgalliumvlwinsys = []

amd_no_loader = false
if get_option('pipe-loader') == 'amdgpu'
  amd_no_loader = true
endif

if host_machine.system() == 'windows'
  files_libgalliumvlwinsys += files('vl_winsys_win32.c')
elif amd_no_loader
  files_libgalliumvlwinsys += files('vl_winsys_amdgpu.c')
else
  files_libgalliumvlwinsys += files('vl_winsys_drm.c')
endif
if with_platform_x11
  files_libgalliumvlwinsys += files('vl_winsys_dri.c')
  links_libgalliumvlwinsys += libloader_x11
  if with_dri_platform == 'drm'
    vlwinsys_deps += [
      dep_xcb_sync, dep_xcb_present, dep_xshmfence, dep_xcb_xfixes,
      dep_xcb_dri3,
    ]
    files_libgalliumvlwinsys += files('vl_winsys_dri3.c')
  endif
endif
if with_gallium_zink
  files_libgalliumvlwinsys += files('vl_winsys_kopper.c')
endif

if with_glx == 'xlib'
  files_libgalliumvlwinsys += files('vl_winsys_xlib_swrast.c')
elif with_gallium_drisw_kms
  files_libgalliumvlwinsys += files('vl_winsys_dri_vgem.c')
endif

inc_libgalliumvlwinsys = [inc_gallium, inc_gallium_aux, inc_include, inc_src]

if is_variable('inc_loader')
  inc_libgalliumvlwinsys += inc_loader
endif

if amd_no_loader
  inc_libgalliumvlwinsys += include_directories('../../../drivers/radeonsi')
  vlwinsys_deps = null_dep
  links_libgalliumvlwinsys = libradeonsi
else
  inc_libgalliumvlwinsys += include_directories('../../../drivers/zink')
endif

# XXX: The dependencies here may be off...
libgalliumvlwinsys = static_library(
  'galliumvlwinsys',
  files_libgalliumvlwinsys,
  include_directories : inc_libgalliumvlwinsys,
  dependencies : [dep_libdrm, vlwinsys_deps, idep_mesautil],
  link_with : links_libgalliumvlwinsys,
  build_by_default : false,
)
