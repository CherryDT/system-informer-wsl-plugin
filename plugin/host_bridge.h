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
    typedef enum WSL_STRING_SETTING
    {
        WslPreferencesSetting,
        WslPathOverridesSetting,
        WslSavedProcessSchedulingSetting,
        WslStringSettingCount
    } WSL_STRING_SETTING;
    /* Includes the terminating WCHAR, matching the saved scheduling limit. */
#define WSL_SETTING_MAXIMUM_BYTES (1024 * 1024)
    typedef struct WSL_HOST_STRING
    {
        PCWSTR Buffer;
        SIZE_T Length; /* WCHARs, excluding the terminator. */
        void *Reference;
    } WSL_HOST_STRING;
    /* Each successful get owns a host reference until the matching release.
     * The returned buffer is immutable and remains valid for that lifetime. */
    BOOL WslHostGetStringSetting(WSL_STRING_SETTING setting, WSL_HOST_STRING *value);
    void WslHostReleaseStringSetting(WSL_HOST_STRING *value);
    BOOL WslHostSetStringSetting(WSL_STRING_SETTING setting, PCWSTR value, SIZE_T length);
    void WslSavedSchedulingSettingsChanged(void);
    void WslOpenHostOptions(HWND owner);
    void WslHostViewSettingsChanged(void);
    void WslPositionDialog(HWND window, HWND owner);
    typedef void(CALLBACK *WSL_SEARCH_CALLBACK)(ULONG_PTR match, void *context);
    void WslCreateSearch(HWND parent, HWND edit, PCWSTR banner, WSL_SEARCH_CALLBACK callback, void *context);
    BOOL WslSearchMatches(ULONG_PTR match, PCWSTR text);

#ifdef __cplusplus
}
#endif
