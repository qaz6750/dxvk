qcdx_abi = meson.get_external_property('qcdx_abi_include_dir')
native9 = []
umd_objects = qcdx_d3d11_objects
if get_option('enable_d3d9')
  native9 = files('umd_d3d9_adapter.cpp', 'umd_d3d9_device.cpp', 'umd_d3d9_backend.cpp')
  umd_objects = [umd_objects, qcdx_d3d9_objects]
endif
shared_library('qcdx_dxvk', native9,
  'umd_adapter.cpp', 'umd_ddi.cpp', 'umd_allocation.cpp',
  'umd_backend.cpp', 'umd_renderer.cpp', 'umd_shader.cpp', 'umd_shader11.cpp',
  wrc_generator.process('version.rc'),
  objects             : umd_objects,
  dependencies        : [dxvk_dep, dxbc_spirv_dep, lib_gdi32, lib_dxgi],
  include_directories : [dxvk_include_path, include_directories(qcdx_abi)],
  vs_module_defs      : 'qcdx_dxvk.def',
  name_prefix         : '',
  install             : true,
)
