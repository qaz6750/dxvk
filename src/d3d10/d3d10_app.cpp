#include <d3d10_1.h>
#include <d3d11.h>
#include "../dxgi/dxgi_interfaces.h"

extern "C" HRESULT WINAPI D3D10CoreCreateDevice(
  IDXGIFactory*, IDXGIAdapter*, UINT, D3D_FEATURE_LEVEL, ID3D10Device**);

namespace {
  template<typename Device>
  HRESULT createDevice(IDXGIAdapter* adapter, D3D10_DRIVER_TYPE driver, HMODULE software,
      UINT flags, D3D_FEATURE_LEVEL level, bool validVersion, DXGI_SWAP_CHAIN_DESC* desc,
      IDXGISwapChain** swapChain, Device** output) {
    using dxvk::Com;
    if (output) *output = nullptr;
    if (swapChain) *swapChain = nullptr;
    if (!validVersion || (swapChain && !desc) || (adapter && driver != D3D10_DRIVER_TYPE_HARDWARE))
      return E_INVALIDARG;
    if (driver != D3D10_DRIVER_TYPE_HARDWARE || software)
      return DXGI_ERROR_UNSUPPORTED;

    Com<IDXGIAdapter> selected = adapter;
    Com<IDXGIFactory> factory;
    HRESULT hr;
    if (selected) {
      hr = selected->GetParent(IID_PPV_ARGS(&factory));
    } else {
      hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
      if (SUCCEEDED(hr))
        hr = factory->EnumAdapters(0, &selected);
    }
    if (FAILED(hr)) return hr;

    Com<ID3D10Device> device;
    hr = D3D10CoreCreateDevice(factory.ptr(), selected.ptr(), flags, level, &device);
    if (FAILED(hr)) return hr;
    Com<Device> typed;
    hr = device->QueryInterface(IID_PPV_ARGS(&typed));
    if (FAILED(hr)) return hr;
    if (swapChain) {
      hr = factory->CreateSwapChain(typed.ptr(), desc, swapChain);
      if (FAILED(hr)) return hr;
    }
    if (output) *output = typed.ref();
    return output || swapChain ? S_OK : S_FALSE;
  }
}

// The ordinary DXVK target exposes D3D10Core to a D3D10 runtime. The versioned
// application DLL also accepts the public device-creation calls directly.
extern "C" {
  HRESULT WINAPI D3D10CreateDeviceAndSwapChain(
      IDXGIAdapter* adapter, D3D10_DRIVER_TYPE driver, HMODULE software, UINT flags,
      UINT sdk, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain, ID3D10Device** device) {
    return createDevice(adapter, driver, software, flags, D3D_FEATURE_LEVEL_10_0,
      sdk == D3D10_SDK_VERSION, desc, swapChain, device);
  }
  HRESULT WINAPI D3D10CreateDevice(
      IDXGIAdapter* adapter, D3D10_DRIVER_TYPE driver, HMODULE software, UINT flags,
      UINT sdk, ID3D10Device** device) {
    return D3D10CreateDeviceAndSwapChain(adapter, driver, software, flags, sdk, nullptr, nullptr, device);
  }
  HRESULT WINAPI D3D10CreateDeviceAndSwapChain1(
      IDXGIAdapter* adapter, D3D10_DRIVER_TYPE driver, HMODULE software, UINT flags,
      D3D10_FEATURE_LEVEL1 level, UINT sdk, DXGI_SWAP_CHAIN_DESC* desc,
      IDXGISwapChain** swapChain, ID3D10Device1** device) {
    return createDevice(adapter, driver, software, flags, D3D_FEATURE_LEVEL(level),
      sdk == D3D10_1_SDK_VERSION, desc, swapChain, device);
  }
  HRESULT WINAPI D3D10CreateDevice1(
      IDXGIAdapter* adapter, D3D10_DRIVER_TYPE driver, HMODULE software, UINT flags,
      D3D10_FEATURE_LEVEL1 level, UINT sdk, ID3D10Device1** device) {
    return D3D10CreateDeviceAndSwapChain1(adapter, driver, software, flags, level, sdk, nullptr, nullptr, device);
  }
}
