#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

class HrError : public std::runtime_error {
public:
    HrError(HRESULT hr, std::string_view what)
        : std::runtime_error(std::format("{} failed: 0x{:08X}", what, static_cast<unsigned>(hr)))
        , hr_(hr) {}

    HRESULT code() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

inline void ThrowIfFailed(HRESULT hr, std::string_view what) {
    if (FAILED(hr)) {
        throw HrError(hr, what);
    }
}

// Directory holding the running executable. Shaders and the staged Agility
// runtime are located relative to it, so the game does not care about the
// working directory it was launched from.
std::filesystem::path ExecutableDirectory();

std::vector<std::byte> ReadBinaryFile(const std::filesystem::path& path);
