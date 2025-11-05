# Copyright © 2017-2019 Intel Corporation
# SPDX-License-Identifier: MIT

libva_version = dep_va.version().split('.')

va_flags = []

libva_files = [
    'buffer.c', 'config.c', 'context.c', 'display.c',
    'picture.c', 'picture_h264.c', 'picture_hevc.c',
    'picture_mjpeg.c', 'picture_vp9.c','picture_av1.c',
    'surface.c', 'decode.c'
]

if with_amd_decode_only
  libva_version = ['1', '0']
  libva_files += 'decode_stub.c'
else
  libva_files += [
    'image.c', 'postproc.c', 'subpicture.c',
  ]
  va_flags += ['-DVA_POSTPROC', '-DVA_COMPOSITOR']
endif

if _codecs.contains('mpeg12dec')
  libva_files += 'picture_mpeg12.c'
endif
if _codecs.contains('mpeg4dec')
  libva_files += 'picture_mpeg4.c'
endif
if _codecs.contains('vc1dec')
  libva_files += 'picture_vc1.c'
endif

has_encode = false
if _codecs.contains('h264enc')
  libva_files += 'picture_h264_enc.c'
  has_encode = true
endif
if _codecs.contains('h265enc')
  libva_files += 'picture_hevc_enc.c'
  has_encode = true
endif
if _codecs.contains('av1enc')
  libva_files += 'picture_av1_enc.c'
  has_encode = true
endif

if has_encode
  libva_files += 'encode.c'
  va_flags += '-DVA_ENCODER'
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
