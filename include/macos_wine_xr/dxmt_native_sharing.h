#pragma once

#include <stdint.h>

#ifdef _WIN32
#include <d3d11_4.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MACOS_WINE_XR_NATIVE_SHARE_NAME_SIZE 54

struct macos_wine_xr_native_share_name
{
	char value[MACOS_WINE_XR_NATIVE_SHARE_NAME_SIZE];
};

#ifdef _WIN32
HRESULT
macos_wine_xr_get_dxmt_texture_share_name(ID3D11DeviceChild *resource,
                                          struct macos_wine_xr_native_share_name *out_name);

HRESULT
macos_wine_xr_get_dxmt_fence_share_name(ID3D11Fence *fence,
                                        struct macos_wine_xr_native_share_name *out_name);
#endif

#ifdef __cplusplus
}
#endif
