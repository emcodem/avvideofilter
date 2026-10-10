project('avvideofilter', 'c',
  version : '0.1.0',
  license : 'LGPL-2.1-or-later',
  meson_version : '>= 0.62',
  default_options : ['warning_level=2', 'c_std=gnu11', 'buildtype=debugoptimized'])

gst_req = '>= 1.20'
gst_dep       = dependency('gstreamer-1.0', version : gst_req)
gstbase_dep   = dependency('gstreamer-base-1.0', version : gst_req)
gstvideo_dep  = dependency('gstreamer-video-1.0', version : gst_req)
ffsuffix      = get_option('ffmpeg_suffix')
avfilter_dep  = dependency('libavfilter' + ffsuffix)
avutil_dep    = dependency('libavutil' + ffsuffix)

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

cdata = configuration_data()
cdata.set_quoted('PACKAGE', meson.project_name())
release_version = get_option('release_version')
cdata.set_quoted('VERSION', release_version != '' ? release_version : meson.project_version())
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('GST_PACKAGE_NAME', 'avvideofilter')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://github.com/emcodem')
configure_file(output : 'config.h', configuration : cdata)

library('gstavvideofilter',
  'src/gstavvideofilter.c',
  c_args : ['-DHAVE_CONFIG_H', '-Wno-unused-parameter'],
  include_directories : include_directories('.'),
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, avfilter_dep, avutil_dep],
  # only the plugin entry points (GST_PLUGIN_DEFINE marks them) are exported
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir : plugins_install_dir,
  install_rpath : get_option('ffmpeg_rpath'))
