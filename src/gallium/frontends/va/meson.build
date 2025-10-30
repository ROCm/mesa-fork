# Copyright © 2017-2019 Intel Corporation
# SPDX-License-Identifier: MIT

libva_version = dep_va.version().split('.')

if with_amd_decode_only
  libva_version = ['1', '0']
endif

libva_files = [
    'buffer.c', 'config.c', 'context.c', 'display.c', 'image.c', 'picture.c',
    'picture_h264.c', 'picture_hevc.c',
    'picture_mjpeg.c', 'picture_vp9.c','picture_av1.c',
    'subpicture.c', 'surface.c',
    'decode.c'
]

if not with_amd_decode_only
  libva_files += [
    'picture_mpeg12.c', 'picture_mpeg4.c',
    'postproc.c',
    'picture_h264_enc.c', 'picture_hevc_enc.c',
    'picture_av1_enc.c', 'encode.c'
  ]
endif

libva_st = static_library(
  'va_st',
  files(libva_files),
  c_args : [
    '-DVA_DRIVER_INIT_FUNC=__vaDriverInit_@0@_@1@'.format(
      libva_version[0], libva_version[1]
    ),
  ],
  gnu_symbol_visibility : 'hidden',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [
    dep_va_headers, dep_x11_xcb, dep_xcb, dep_xcb_dri3,
    dep_libdrm, idep_mesautil,
  ],
)
