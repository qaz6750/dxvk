#include "umd_renderer.h"
#include <dxgi.h>
#include <vulkan/vk_icd.h>

namespace dxvk::umd {
// DXGI loads the UMD by absolute path from DriverStore. Keep its paired ICD
// loaded for the full lifetime of the Vulkan instance and all device objects.
extern "C" IMAGE_DOS_HEADER __ImageBase;
static HRESULT loadDriver(Rc<vk::LibraryFn>& library) {
  std::wstring path(32768, L'\0');
  DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase),
    path.data(), DWORD(path.size()));
  if (!length || length >= path.size()) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  path.resize(length);
  auto slash = path.rfind(L'\\');
  if (slash == path.npos) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
#if defined(_M_IX86) || defined(__i386__)
  path.replace(slash + 1, path.npos, L"vulkan_freedreno_x86.dll");
#else
  path.replace(slash + 1, path.npos, L"vulkan_freedreno.dll");
#endif
  struct Module {
    HMODULE value;
    ~Module() { if (value) FreeLibrary(value); }
  } module{LoadLibraryExW(path.c_str(), nullptr,
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
  if (!module.value) return HRESULT_FROM_WIN32(GetLastError());
  auto negotiate = reinterpret_cast<PFN_vk_icdNegotiateLoaderICDInterfaceVersion>(
    GetProcAddress(module.value, "vk_icdNegotiateLoaderICDInterfaceVersion"));
  auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
    GetProcAddress(module.value, "vk_icdGetInstanceProcAddr"));
  uint32_t version = CURRENT_LOADER_ICD_INTERFACE_VERSION;
  if (!negotiate || !get || negotiate(&version) != VK_SUCCESS || version < 4)
    return DXGI_ERROR_UNSUPPORTED;
  library = new vk::LibraryFn(get, module.value);
  module.value = nullptr;
  return S_OK;
}

HRESULT openRenderer(const LUID& luid, DxvkInstanceFlags flags, Rc<DxvkInstance>& instance,
                     Rc<DxvkAdapter>& adapter) {
  DxvkInstanceImportInfo args = {};
  HRESULT hr = loadDriver(args.nativeDriverLibrary);
  if (FAILED(hr)) return hr;
  instance = new DxvkInstance(args, flags);
  adapter = instance->findAdapterByLuid(&luid);
  return adapter && adapter->info().driverId == VK_DRIVER_ID_MESA_TURNIP ? S_OK : DXGI_ERROR_NOT_FOUND;
}
}
