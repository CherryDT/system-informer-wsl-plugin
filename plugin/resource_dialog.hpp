#pragma once
#include "common.hpp"

namespace wsl
{
constexpr UINT ResourceActionCompleted = WM_APP + 88;
struct ResourceInput
{
    std::wstring label;
    std::string key;
    std::wstring value;
};

// Each tool owns its asynchronous mailbox, so closing it never waits for WSL.
void openResourceTool(HWND owner, const std::wstring &distro, Json request, const std::wstring &title,
                      std::vector<ResourceInput> inputs = {}, bool mutation = false);
// Display locally cached properties through the same renderer without a WSL request.
void openResourceReport(HWND owner, const std::wstring &title, Json response);
} // namespace wsl
