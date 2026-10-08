#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* The host owns refresh policy and highlighting settings. These adapters keep
     * System Informer SDK declarations out of the C++ presentation code. */
    DWORD WslHostRefreshInterval(void);
    BOOL WslHostRefreshAutomatically(void);
    DWORD WslHostIntegerSetting(PCWSTR name);

#ifdef __cplusplus
}
#endif
