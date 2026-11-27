// Based on the embedded backend by sunflower2333 (Kancy Joe).
#include "umd_api.h"
#include <string>
#include "../dxvk/dxvk_instance.h"
#include "../d3d11/d3d11_context_imm.h"
#include "../d3d11/d3d11_device.h"
#include <vulkan/vk_icd.h>

namespace dxvk {
Logger Logger::s_instance("");
}

namespace dxvk::umd {
HRESULT flushRuntimeSubmission(ID3D11DeviceContext* context) noexcept {
  try { return static_cast<D3D11ImmediateContext*>(context)->FlushRuntimeSubmission(); }
  catch (...) { return DXGI_ERROR_DEVICE_REMOVED; }
}
HRESULT isStagingResourceBusy(ID3D11DeviceContext* context, ID3D11Resource* resource, BOOL* busy) noexcept {
  try { return static_cast<D3D11ImmediateContext*>(context)->IsStagingResourceBusy(resource, busy); }
  catch (...) { return DXGI_ERROR_DEVICE_REMOVED; }
}

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
  path.replace(slash + 1, path.npos, L"vulkan_freedreno.dll");
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

HRESULT createDevice(const LUID& luid, D3D_FEATURE_LEVEL level,
                     ID3D11Device** device, ID3D11DeviceContext** context) noexcept {
  if (!device || !context) return E_POINTER;
  *device = nullptr;
  *context = nullptr;
  try {
    DxvkInstanceImportInfo args = {};
    auto loaded = loadDriver(args.nativeDriverLibrary);
    if (FAILED(loaded)) return loaded;
    Rc<DxvkInstance> instance = new DxvkInstance(args, 0);
    auto adapter = instance->findAdapterByLuid(&luid);
    if (!adapter || adapter->info().driverId != VK_DRIVER_ID_MESA_TURNIP) return DXGI_ERROR_NOT_FOUND;
    auto renderer = adapter->createDevice();
    level = implementationFeatureLevel(level);
    if (level > D3D11Device::GetMaxFeatureLevel(*renderer)) return DXGI_ERROR_UNSUPPORTED;
    Com<D3D11DXGIDevice> container = new D3D11DXGIDevice(nullptr, nullptr, nullptr,
      instance, adapter, renderer, level, 0);
    auto hr = container->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(device));
    if (SUCCEEDED(hr)) (*device)->GetImmediateContext(context);
    return hr;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (const DxvkError& e) {
      Logger::err(str::format("qcdx: ", e.message()));
      return DXGI_ERROR_UNSUPPORTED;
    } catch (...) { return E_FAIL; }
}
}
