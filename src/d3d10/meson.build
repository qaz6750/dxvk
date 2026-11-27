module_name = qcdx_apps ? 'qcdx_dxvk10' : dxvk_name_prefix + 'd3d10core'
qcdx_rc_args = qcdx_apps ? [rc_define + 'DXVK_DLL_NAME="' + module_name + '.dll"'] : []
d3d10_core_res = wrc_generator.process('version10_core.rc', extra_args : qcdx_rc_args)

d3d10_core_src = [
  'd3d10_core.cpp',
]

if qcdx_apps
  d3d10_core_src += ['d3d10_app.cpp']
endif

d3d10_core_ld_args      = []
d3d10_core_link_depends = []

if platform == 'windows'
  d3d10_d3d11_dep = qcdx_apps ? [d3d11_dep, dxgi_dep] : [lib_d3d11]
else
  d3d10_core_ld_args      += [ '-Wl,--version-script', join_paths(meson.current_source_dir(), 'd3d10core.sym') ]
  d3d10_core_link_depends += files('d3d10core.sym')
  d3d10_d3d11_dep = d3d11_dep
endif

d3d10_core_dll = shared_library(module_name, d3d10_core_src, d3d10_core_res,
  dependencies        : [ d3d10_d3d11_dep ],
  include_directories : dxvk_include_path,
  install             : true,
  vs_module_defs      : qcdx_apps ? 'qcdx_dxvk10.def' : 'd3d10core'+def_spec_ext,
  link_args           : d3d10_core_ld_args,
  link_depends        : [ d3d10_core_link_depends ],
  kwargs              : dxvk_so_version,
)

d3d10_core_dep = declare_dependency(
  link_with           : [ d3d10_core_dll ],
)

if platform != 'windows'
  pkg.generate(d3d10_core_dll,
    filebase: dxvk_pkg_prefix + 'd3d10core',
    subdirs:  'dxvk',
  )
endif
