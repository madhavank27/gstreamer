libcamera_gst_sources = [
    'gstlibcamera.c',
    'gstlibcamera-utils.cpp',
    'gstlibcamerasrc.cpp',
    'gstlibcameraprovider.cpp',
    'gstlibcamerapad.cpp',
    'gstlibcameraallocator.cpp',
    'gstlibcamerapool.cpp'
]

libcamera_gst_c_args = [
    '-DVERSION="@0@"'.format(libcamera_git_version),
    '-DPACKAGE="@0@"'.format(meson.project_name()),
]

gst_req = '>=1.16.1'
gstvideo_dep = dependency('gstreamer-video-1.0', version : gst_req,
    required : get_option('gstreamer'))
gstallocator_dep = dependency('gstreamer-allocators-1.0', version : gst_req,
    required : get_option('gstreamer'))

if gstvideo_dep.found() and gstallocator_dep.found()
  libcamera_gst = shared_library('gstlibcamera',
      libcamera_gst_sources,
      c_args : libcamera_gst_c_args,
      include_directories : [],
      dependencies : [libcamera_dep, gstvideo_dep, gstallocator_dep],
      install: true,
      install_dir : '@0@/gstreamer-1.0'.format(get_option('libdir')),
  )
endif
