module_name = qcdx_apps ? 'qcdx_dxvk8' : dxvk_name_prefix + 'd3d8'
qcdx_rc_args = qcdx_apps ? [rc_define + 'DXVK_DLL_NAME="' + module_name + '.dll"'] : []
d3d8_res = wrc_generator.process('version.rc', extra_args : qcdx_rc_args)

d3d8_src = [
  'd3d8_buffer.cpp',
  'd3d8_device.cpp',
  'd3d8_interface.cpp',
  'd3d8_main.cpp',
  'd3d8_options.cpp',
  'd3d8_shader.cpp',
  'd3d8_state_block.cpp',
  'd3d8_surface.cpp',
  'd3d8_swapchain.cpp',
  'd3d8_texture.cpp',
  'd3d8_volume.cpp'
]

d3d8_cpp_args = []
if platform == 'windows' and dxvk_is_msvc and not cpp.has_header('d3d8.h')
  # Recent Windows SDKs omit D3D8. Expose just the three bundled D3D8
  # headers here so D3D9 and the rest of the SDK use their native headers.
  foreach header : ['d3d8.h', 'd3d8caps.h', 'd3d8types.h']
    configure_file(input : '../../include/native/directx/' + header,
      output : header, copy : true)
  endforeach
  d3d8_cpp_args += ['-DDXVK_BUNDLED_D3D8']
endif

d3d8_ld_args      = []
d3d8_link_depends = []

d3d8_d3d9_dep = platform != 'windows' or qcdx_apps ? d3d9_dep : lib_d3d9
if platform != 'windows'
  d3d8_ld_args      += [ '-Wl,--version-script', join_paths(meson.current_source_dir(), 'd3d8.sym') ]
  d3d8_link_depends += files('d3d8.sym')
endif

d3d8_dll = shared_library(module_name, d3d8_src, d3d8_res,
  cpp_args            : d3d8_cpp_args,
  dependencies        : [ d3d8_d3d9_dep, util_dep, dxbc_spirv_dep, dxvk_dep ],
  include_directories : dxvk_include_path,
  install             : true,
  vs_module_defs      : 'd3d8'+def_spec_ext,
  link_args           : d3d8_ld_args,
  link_depends        : [ d3d8_link_depends ],
  kwargs              : dxvk_so_version,
)

d3d8_dep = declare_dependency(
  link_with           : [ d3d8_dll ],
  include_directories : [ dxvk_include_path ],
)

if platform != 'windows'
  pkg.generate(d3d8_dll,
    filebase: dxvk_pkg_prefix + 'd3d8',
    subdirs:  'dxvk',
  )
endif
