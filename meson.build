project(
  'cmsgpack',
  'c',
  version: '0.1.2',
  meson_version: '>=1.2.0',
  default_options: [
    'buildtype=release',
  ],
)

python = import('python')
py_installation = python.find_installation(pure: false)

py_installation.extension_module(
  'cmsgpack',
  sources : ['cmsgpack/cmsgpack.c'],
  include_directories : include_directories('cmsgpack'),
  install : true,
  subdir : 'cmsgpack',
)

install_data('cmsgpack/__init__.py', install_dir : py_installation.get_install_dir() / 'cmsgpack')
install_data('cmsgpack/cmsgpack.pyi', install_dir : py_installation.get_install_dir() / 'cmsgpack')

