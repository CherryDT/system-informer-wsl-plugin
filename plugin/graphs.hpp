#pragma once
#include "view_state.hpp"

namespace wsl::ui
{
constexpr UINT GraphSampleChanged = WM_APP + 99;
HWND createHistoryGraph(HWND parent, View &view, bool memory);
} // namespace wsl::ui
