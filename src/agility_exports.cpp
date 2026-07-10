// Agility SDK opt-in.
//
// The system's d3d12.dll looks for these two exported symbols in the running
// executable. When it finds them it loads D3D12Core.dll from D3D12SDKPath
// (relative to the executable) instead of using the D3D12 runtime that shipped
// with the installed copy of Windows.
//
// Constraints worth remembering, because violating them fails silently at
// runtime rather than at build time:
//   * They must live in the executable, not in a DLL it links.
//   * They must be exported, hence __declspec(dllexport).
//   * D3D12SDKPath is relative to the .exe and needs a trailing separator.
//   * The version must match the D3D12Core.dll actually staged on disk;
//     CMake keeps the two in sync via AGILITY_SDK_INTERFACE_VERSION.
//
// See: https://devblogs.microsoft.com/directx/directx12agility/

#include <windows.h>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = AGILITY_SDK_INTERFACE_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
