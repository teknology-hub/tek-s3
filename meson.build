project(
  'tek-s3',
  'c', 'cpp',
  version: files('meson.version'), meson_version: '>=1.3.2',
  license: 'GPL-3.0-or-later', license_files: 'COPYING',
  default_options: {
    'cpp_std': 'gnu++23',
    'warning_level': '3'
  }
)
is_windows = host_machine.system() == 'windows'
if is_windows
  add_project_arguments('-DUNICODE', '-D_UNICODE', language: ['c', 'cpp'])
  add_project_link_arguments('-municode', language: ['c', 'cpp'])
  # Make file version for the .rc file
  rc_version = meson.project_version().replace('.', ',')
  if rc_version.contains('-')
    rc_version = rc_version.split('-')[0]
  endif
  num_elems = rc_version.split(',').length()
  if num_elems == 2
    rc_version += ',0,0'
  endif
  if num_elems == 3
    rc_version += ',0'
  endif
else # is_windows
  add_project_arguments('-D_GNU_SOURCE', language: ['c', 'cpp'])
endif # is_windows else
compiler = meson.get_compiler('c')
# -Wno-attributes: Project code uses many attributes that are supported only by
#    specific compilers or even specific compiler versions
# -Wno-nullability-extension: Fallbacks for these attributes are provided by
#    null_attrs.h anyway
# -Wno-pointer-arith: provided by GNU extensions (gcc)
# -Wno-gnu-binary-literal: provided by GNU extensions (clang)
# -Wno-gnu-pointer-arith: provided by GNU extensions (clang)
add_project_arguments(
  compiler.get_supported_arguments(
    '-Wno-attributes', '-Wno-nullability-extension', '-Wno-pointer-arith',
    '-Wno-gnu-binary-literal', '-Wno-gnu-pointer-arith'),
  language: ['c', 'cpp']
)
# -Wno-ignored-attributes: gcc emits these on decltype(&function)
add_project_arguments(
  compiler.get_supported_arguments('-Wno-ignored-attributes'),
  language: 'cpp'
)
zlib_dep = dependency('zlib-ng', required: get_option('zlib_ng'))
if not zlib_dep.found()
  zlib_dep = dependency('zlib')
  add_project_arguments('-DZLIB_CONST', language: ['c', 'cpp'])
endif
libbrotlienc_dep = dependency('libbrotlienc', required: get_option('brotli'))
libsystemd_dep = dependency('libsystemd', required: get_option('systemd'))
libzstd_dep = dependency('libzstd', required: get_option('zstd'))
configure_file(
  output: 'config.h',
  configuration: {
    'TEK_S3_VERSION': '"' + meson.project_version() + '"',
    'TEK_S3B_ZNG': zlib_dep.name() == 'zlib-ng',
    'TEK_S3B_BROTLI': libbrotlienc_dep.found(),
    'TEK_S3B_SYSTEMD': libsystemd_dep.found(),
    'TEK_S3B_ZSTD': libzstd_dep.found()
  }
)
override_options = {
  # Meson supports c23/gnu23 only since 1.4.0
  'c_std': meson.version().version_compare('<1.4.0') ? 'gnu2x' : 'gnu23'
}
deps = [
  dependency('RapidJSON'),
  libbrotlienc_dep,
  libsystemd_dep,
  dependency('libwebsockets'),
  libzstd_dep,
  dependency('tek-steamclient'),
  zlib_dep,
  subproject('ValveFileVDF').get_variable('valve_file_vdf_dep')
]
src = [
  'src/cm_callbacks.cpp',
  is_windows ? [
    'src/main_windows.c',
    'src/os_windows.c'
  ] : [
    'src/main_linux.c',
    'src/os_linux.c'
  ],
  'src/manifest.cpp',
  'src/server.cpp',
  'src/signin.cpp',
  'src/state.cpp',
  'src/utils.c'
]
if is_windows
  configure_file(
    input: 'res/tek-s3.manifest.in',
    output: 'tek-s3.manifest',
    configuration: {'version': rc_version.replace(',', '.')}
  )
  src += import('windows').compile_resources(
    configure_file(
      input: 'res/tek-s3.rc.in',
      output: 'tek-s3.rc',
      configuration: {
        'file_version': rc_version,
        'pretty_version': meson.project_version()
      }
    )
  )
endif # is_windows
executable(
  'tek-s3',
  src,
  dependencies: deps,
  gnu_symbol_visibility: 'hidden',
  install: true,
  override_options: override_options
)
systemd_dep = dependency('systemd', required: get_option('systemd'))
if systemd_dep.found()
  configure_file(
    input: 'tek-s3.service.in', output: 'tek-s3.service',
    configuration: {'prefix': get_option('prefix')},
    install: true,
    install_dir: systemd_dep.get_variable(pkgconfig: 'systemdsystemunitdir')
  )
endif
