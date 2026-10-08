#pragma once
#include <windows.h>

#define WSL_VIEW_SETTINGS_CHANGED (WM_APP + 86)

#ifdef __cplusplus
extern "C"
{
#endif

    /* The host owns refresh policy and highlighting settings. These adapters keep
     * System Informer SDK declarations out of the C++ presentation code. */
    DWORD WslHostRefreshInterval(void);
    BOOL WslHostRefreshAutomatically(void);
    DWORD WslHostIntegerSetting(PCWSTR name);
    void WslOpenHostOptions(HWND owner);
    void WslHostViewSettingsChanged(void);
    void WslPositionDialog(HWND window, HWND owner);
    typedef void(CALLBACK *WSL_SEARCH_CALLBACK)(ULONG_PTR match, void *context);
    void WslCreateSearch(HWND parent, HWND edit, PCWSTR banner, WSL_SEARCH_CALLBACK callback, void *context);
    BOOL WslSearchMatches(ULONG_PTR match, PCWSTR text);

#ifdef __cplusplus
}
#endif
