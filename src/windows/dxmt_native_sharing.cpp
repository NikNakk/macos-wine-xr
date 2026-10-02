#include "macos_wine_xr/dxmt_native_sharing.h"

#include <cstring>

#include "dxmt_native_interop.h"

static HRESULT
get_share_name(ID3D11DeviceChild *object,
               const GUID &key,
               struct macos_wine_xr_native_share_name *out_name)
{
	if (object == nullptr || out_name == nullptr) {
		return E_INVALIDARG;
	}

	std::memset(out_name, 0, sizeof(*out_name));
	UINT size = sizeof(out_name->value);
	HRESULT hr = object->GetPrivateData(key, &size, out_name->value);
	if (FAILED(hr)) {
		return hr;
	}

	if (size == 0 || size > sizeof(out_name->value)) {
		std::memset(out_name, 0, sizeof(*out_name));
		return E_FAIL;
	}

	out_name->value[sizeof(out_name->value) - 1] = '\0';
	return out_name->value[0] != '\0' ? S_OK : E_FAIL;
}

extern "C" HRESULT
macos_wine_xr_get_dxmt_texture_share_name(ID3D11DeviceChild *resource,
                                          struct macos_wine_xr_native_share_name *out_name)
{
	return get_share_name(resource, DXMT_GUID_SHARED_TEXTURE_BOOTSTRAP_NAME, out_name);
}

extern "C" HRESULT
macos_wine_xr_get_dxmt_fence_share_name(ID3D11Fence *fence,
                                        struct macos_wine_xr_native_share_name *out_name)
{
	return get_share_name(fence, DXMT_GUID_SHARED_FENCE_BOOTSTRAP_NAME, out_name);
}
