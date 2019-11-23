libcamera_gst_sources = [
    'gstlibcamera.c',
    'gstlibcamerasrc.cpp',
]

libcamera_gst_c_args = [
    '-DVERSION="@0@"'.format(libcamera_git_version),
    '-DPACKAGE="@0@"'.format(meson.project_name()),
]

gst_dep = dependency('gstreamer-video-1.0', version : '>=1.16.1',
    required : get_option('gstreamer'))

if gst_dep.found()
  libcamera_gst = shared_library('gstlibcamera',
      libcamera_gst_sources,
      c_args : libcamera_gst_c_args,
      include_directories : [],
      dependencies : [libcamera_dep, gst_dep],
      install: true,
      install_dir : '@0@/gstreamer-1.0'.format(get_option('libdir')),
  )
endif
