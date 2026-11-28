#include "d3d9_device.h"
#include "d3d9_common_buffer.h"
#include "d3d9_interface.h"
#include "d3d9_shader.h"
#include "d3d9_shader_code.h"
#include "d3d9_surface.h"
#include "d3d9_swapchain.h"

namespace dxvk {

  HRESULT D3D9DeviceEx::ClearNative(DWORD count, const D3DRECT* rects, DWORD flags,
      D3DCOLOR color, float depth, DWORD stencil, bool computeRects) {
    if (!m_parent->IsNativeRenderer())
      return D3DERR_INVALIDCALL;
    if (!computeRects && !count)
      return D3D_OK;
    return ClearInternal(count, rects, flags, color, depth, stencil, !computeRects);
  }


  HRESULT D3D9DeviceEx::CreateNativeVertexShader(const DWORD* code, UINT bytes,
                                               IDirect3DVertexShader9** shader) {
    if (!shader || !validateD3D9ShaderCode(code, bytes, true)) return D3DERR_INVALIDCALL;
    D3D9CommonShader module;
    size_t length = 0;
    const HRESULT hr = CreateShaderModule(&module, &length, D3D9ShaderType::VertexShader, code, bytes);
    if (FAILED(hr)) return hr;
    *shader = ref(new D3D9VertexShader(this, &m_shaderAllocator, module, code, uint32_t(length)));
    return S_OK;
  }


  HRESULT D3D9DeviceEx::CreateNativePixelShader(const DWORD* code, UINT bytes,
                                              IDirect3DPixelShader9** shader) {
    if (!shader || !validateD3D9ShaderCode(code, bytes, false)) return D3DERR_INVALIDCALL;
    D3D9CommonShader module;
    size_t length = 0;
    const HRESULT hr = CreateShaderModule(&module, &length, D3D9ShaderType::PixelShader, code, bytes);
    if (FAILED(hr)) return hr;
    *shader = ref(new D3D9PixelShader(this, &m_shaderAllocator, module, code, uint32_t(length)));
    return S_OK;
  }


  HRESULT D3D9DeviceEx::SetNativeRenderTarget(IDirect3DSurface9* target) {
    D3D9DeviceLock lock = LockDevice();
    if (!m_parent->IsNativeRenderer()) return D3DERR_INVALIDCALL;
    auto surface = static_cast<D3D9Surface*>(target);
    if (surface && surface->GetDevice() != this)
      return D3DERR_INVALIDCALL;
    return SetRenderTargetInternal(0, target);
  }


  HRESULT D3D9DeviceEx::SetNativeWInfo(float wNear, float wFar) {
    D3D9DeviceLock lock = LockDevice();
    if (!IsNativeRenderer()) return D3DERR_INVALIDCALL;
    m_nativeWInfoValid = true;
    m_nativeFogUseZ = wNear == 1.0f && wFar == 1.0f;
    m_dirty.set(D3D9DeviceDirtyFlag::Fog);
    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::InitializeNativeOffscreen() {
    D3D9DeviceLock lock = LockDevice();
    if (!m_parent->IsNativeRenderer() || m_implicitSwapchain != nullptr)
      return D3DERR_INVALIDCALL;

    D3DPRESENT_PARAMETERS defaults = {};
    defaults.Windowed = TRUE;
    ResetState(&defaults);
    const D3DVIEWPORT9 viewport = {0, 0, 1, 1, 0.0f, 1.0f};
    const RECT scissor = {0, 0, 1, 1};
    SetViewport(&viewport);
    SetScissorRect(&scissor);
    return FlushRuntimeSubmission();
  }


  bool D3D9DeviceEx::IsNativeRenderer() const noexcept {
    return m_parent->IsNativeRenderer();
  }


  HRESULT D3D9DeviceEx::CopyNativeBuffer(D3D9CommonBuffer* destination, UINT destinationOffset,
      D3D9CommonBuffer* source, UINT sourceOffset, UINT bytes, const void* upload) {
    D3D9DeviceLock lock = LockDevice();
    if (!IsNativeRenderer() || !destination || !source
        || destination->GetLockCount() || source->GetLockCount()
        || uint64_t(destinationOffset) + bytes > destination->Desc()->Size
        || uint64_t(sourceOffset) + bytes > source->Desc()->Size)
      return D3DERR_INVALIDCALL;
    if (!bytes) return D3D_OK;

    // Preserve bytes outside the requested destination range and make staged
    // source writes visible before the actual GPU-to-GPU copy.
    if (source->NeedsUpload()) FlushBuffer(source);
    if (destination != source && destination->NeedsUpload()) FlushBuffer(destination);
    auto srcBuffer = source->GetBuffer<D3D9_COMMON_BUFFER_TYPE_REAL>();
    VkDeviceSize srcOffset = sourceOffset;
    if (upload) {
      ThrottleAllocation();
      auto staging = AllocStagingBuffer(bytes);
      std::memcpy(staging.mapPtr, upload, bytes);
      srcBuffer = staging.slice.buffer();
      srcOffset = staging.slice.offset();
    }
    auto dstBuffer = destination->GetBuffer<D3D9_COMMON_BUFFER_TYPE_REAL>();
    Rc<DxvkBuffer> dstMapping;
    if (destination->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_BUFFER)
      dstMapping = destination->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>();
    EmitCs([
      cSrcBuffer = std::move(srcBuffer), cSrcOffset = srcOffset,
      cDstBuffer = std::move(dstBuffer), cDstMapping = std::move(dstMapping),
      cDstOffset = destinationOffset, cBytes = bytes
    ](DxvkContext* ctx) {
      if (cDstBuffer == cSrcBuffer)
        ctx->copyBufferRegion(cDstBuffer, cDstOffset, cSrcOffset, cBytes);
      else
        ctx->copyBuffer(cDstBuffer, cDstOffset, cSrcBuffer, cSrcOffset, cBytes);
      if (cDstMapping != nullptr)
        ctx->copyBuffer(cDstMapping, cDstOffset, cDstBuffer, cDstOffset, cBytes);
    });
    destination->SetNeedsReadback(true);
    TrackBufferMappingBufferSequenceNumber(destination);
    // SYSTEMMEM can be consumed by dynamic per-draw CPU uploads immediately.
    // Join its mapping write before returning, even when it has no external
    // runtime pointer. DEFAULT locks retain their usual deferred wait.
    if (destination->Desc()->Pool == D3DPOOL_SYSTEMMEM) {
      WaitForResource(*destination->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>(),
        destination->GetMappingBufferSequenceNumber(), D3DLOCK_READONLY);
      destination->SetNeedsReadback(false);
    }
    ConsiderFlush(GpuFlushType::ImplicitWeakHint);
    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::FlushRuntimeSubmission() {
    D3D9DeviceLock lock = LockDevice();
    ExecuteFlush(true);
    return m_dxvkDevice->getDeviceStatus() == VK_SUCCESS
      ? D3D_OK : D3DERR_DEVICELOST;
  }

}
