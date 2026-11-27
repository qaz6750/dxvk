#pragma once
#include "../dxvk/dxvk_instance.h"

namespace dxvk::umd {
HRESULT openRenderer(const LUID& luid, DxvkInstanceFlags flags, Rc<DxvkInstance>& instance,
                     Rc<DxvkAdapter>& adapter);
}
