#pragma once
#include "common.hpp"

namespace wsl
{
std::wstring processTooltip(const Row &row);
std::wstring serviceTooltip(const Row &row);
std::wstring networkTooltip(const Row &row);
} // namespace wsl
