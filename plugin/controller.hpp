#pragma once
#include "common.hpp"
namespace wsl {
constexpr uintptr_t DiscoverTag = 1;
constexpr uintptr_t SnapshotTag = 2;
constexpr uintptr_t ConnectionsTag = 3;
constexpr uintptr_t ServicesTag = 4;
constexpr uintptr_t ActionTag = 5;
void startController();
void stopController();
void disconnect(const std::wstring& distro);
}
