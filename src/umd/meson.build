qcdx_abi = meson.get_external_property('qcdx_abi_include_dir')
shared_library('qcdx_dxvk',
  'umd_adapter.cpp', 'umd_ddi.cpp', 'umd_allocation.cpp',
  'umd_backend.cpp', 'umd_shader.cpp', 'umd_shader11.cpp',
  wrc_generator.process('version.rc'),
  objects             : qcdx_d3d11_objects,
  dependencies        : [dxvk_dep, dxbc_spirv_dep, lib_gdi32, lib_dxgi],
  include_directories : [dxvk_include_path, include_directories(qcdx_abi)],
  vs_module_defs      : 'qcdx_dxvk.def',
  name_prefix         : '',
  install             : true,
)
