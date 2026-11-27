module_name = qcdx_apps ? 'qcdx_dxvk_dxgi' : dxvk_name_prefix + 'dxgi'
qcdx_rc_args = qcdx_apps ? [rc_define + 'DXVK_DLL_NAME="' + module_name + '.dll"'] : []
dxgi_res = wrc_generator.process('version.rc', extra_args : qcdx_rc_args)

dxgi_src = [
  'dxgi_adapter.cpp',
  'dxgi_enums.cpp',
  'dxgi_factory.cpp',
  'dxgi_format.cpp',
  'dxgi_main.cpp',
  'dxgi_monitor.cpp',
  'dxgi_options.cpp',
  'dxgi_output.cpp',
  'dxgi_surface.cpp',
  'dxgi_swapchain.cpp',
]

dxgi_ld_args      = []
dxgi_link_depends = []
dxgi_extra_dep    = []

if platform == 'windows'
  dxgi_extra_dep    += [ cpp.find_library('version') ]
else
  dxgi_ld_args      += [ '-Wl,--version-script', join_paths(meson.current_source_dir(), 'dxgi.sym') ]
  dxgi_link_depends += files('dxgi.sym')
endif

dxgi_dll = shared_library(module_name, dxgi_src, dxgi_res,
  dependencies        : [ dxvk_dep, dxgi_extra_dep ],
  include_directories : dxvk_include_path,
  install             : true,
  vs_module_defs      : 'dxgi'+def_spec_ext,
  link_args           : dxgi_ld_args,
  link_depends        : [ dxgi_link_depends ],
  kwargs              : dxvk_so_version,
)

dxgi_dep = declare_dependency(
  link_with           : [ dxgi_dll ],
  include_directories : [ dxvk_include_path ],
)

if platform != 'windows'
  pkg.generate(dxgi_dll,
    filebase: dxvk_pkg_prefix + 'dxgi',
    subdirs:  'dxvk',
  )
endif
