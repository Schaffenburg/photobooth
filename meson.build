project('photobooth', 'c', default_options: [
  'c_std=gnu99',
  'warning_level=2'
])

deps = [
  dependency('gstreamer-1.0', version : '>= 1.16.0'),
  dependency('gstreamer-video-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('libgphoto2', version : '>= 2.5.10'),
  dependency('gtk+-3.0', version : '>= 3.24.0'),
  dependency('libcurl'),
  dependency('x11'),
  dependency('libcanberra-gtk3'),
  dependency('json-glib-1.0'),
]

gnome = import('gnome')
photoboothresources = gnome.compile_resources(
  'photobooth-resources', 'photobooth.gresource.xml',
  source_dir : '.',
  c_name : 'photobooth')

src = [
  'photobooth.c',
  'photoboothwin.c',
  'photoboothled.c',
  'photoboothmasquerade.c',
  'focus.c',
  photoboothresources
]

executable('photobooth', 
  sources: src,
  dependencies: deps,
  link_args: '-rdynamic',
  install: true)
