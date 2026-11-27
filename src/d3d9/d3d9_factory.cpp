#include "d3d9_interface.h"

#include <new>

namespace dxvk {

  HRESULT CreateD3D9(bool extended, IDirect3D9Ex** output,
                     const D3D9ON12_ARGS* overrides, uint32_t overrideCount) {
    if (!output)
      return D3DERR_INVALIDCALL;

    *output = nullptr;
    try {
      *output = ref(new D3D9InterfaceEx(extended, overrides, overrideCount));
      return D3D_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_NOTAVAILABLE;
    } catch (...) {
      return E_FAIL;
    }
  }

}

extern "C" DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9(UINT) {
  IDirect3D9Ex* result = nullptr;
  dxvk::CreateD3D9(false, &result, nullptr, 0);
  return result;
}

extern "C" DLLEXPORT HRESULT __stdcall Direct3DCreate9Ex(UINT, IDirect3D9Ex** output) {
  return dxvk::CreateD3D9(true, output, nullptr, 0);
}
