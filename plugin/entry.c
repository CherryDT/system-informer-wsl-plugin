/* The only translation unit coupled to the System Informer SDK. Keeping this
 * bridge in C lets the UI and transport use ordinary C++ and Win32 headers. */
#include <phdk.h>
#include <settings.h>

extern HWND WslCreateView(HWND parent, HINSTANCE instance);
extern void WslSetActive(BOOL active);
extern void WslShutdown(void);

static HINSTANCE PluginModule;
static PH_CALLBACK_REGISTRATION MainWindowRegistration;
static PH_CALLBACK_REGISTRATION UnloadRegistration;
static HWND ViewWindow;

void WslApplyTheme(HWND window)
{
    PhInitializeWindowTheme(window, !!PhGetIntegerSetting(L"EnableThemeSupport"));
}

BOOL WslIsDarkTheme(void)
{
    return !!PhGetIntegerSetting(L"EnableThemeSupport");
}

HFONT WslGetHostFont(void)
{
    return SystemInformer_GetFont();
}

static BOOLEAN TabCallback(
    PPH_MAIN_TAB_PAGE page,
    PH_MAIN_TAB_PAGE_MESSAGE message,
    PVOID parameter1,
    PVOID parameter2
    )
{
    UNREFERENCED_PARAMETER(page);
    switch (message)
    {
    case MainTabPageCreateWindow:
        ViewWindow = WslCreateView((HWND)parameter2, PluginModule);
        if (parameter1)
            *(HWND*)parameter1 = ViewWindow;
        return ViewWindow != NULL;
    case MainTabPageSelected:
        WslSetActive(parameter1 != NULL);
        return TRUE;
    case MainTabPageFontChanged:
        if (ViewWindow)
            SendMessage(ViewWindow, WM_SETFONT, (WPARAM)parameter1, TRUE);
        return TRUE;
    case MainTabPageDestroy:
        WslShutdown();
        ViewWindow = NULL;
        return TRUE;
    default:
        return FALSE;
    }
}

static VOID NTAPI MainWindowShowing(PVOID parameter, PVOID context)
{
    PH_MAIN_TAB_PAGE page = { 0 };
    UNREFERENCED_PARAMETER(parameter);
    UNREFERENCED_PARAMETER(context);
    PhInitializeStringRef(&page.Name, L"WSL");
    page.Callback = TabCallback;
    PhPluginCreateTabPage(&page);
}

static VOID NTAPI PluginUnloading(PVOID parameter, PVOID context)
{
    UNREFERENCED_PARAMETER(parameter);
    UNREFERENCED_PARAMETER(context);
    /* Join workers here, never under the loader lock in DllMain. */
    WslShutdown();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    PPH_PLUGIN plugin;
    PPH_PLUGIN_INFORMATION information;
    UNREFERENCED_PARAMETER(reserved);
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    PluginModule = instance;
    plugin = PhRegisterPlugin(L"DavidTrapp.WslTools", instance, &information);
    if (!plugin)
        return FALSE;
    information->DisplayName = L"WSL Tools";
    information->Author = L"David Trapp";
    information->Description = L"Processes, connections, open files, modules and systemd services for WSL 2. MIT licensed.";
    information->HasOptions = FALSE;

    PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackMainWindowShowing),
        MainWindowShowing, NULL, &MainWindowRegistration);
    PhRegisterCallback(PhGetPluginCallback(plugin, PluginCallbackUnload),
        PluginUnloading, NULL, &UnloadRegistration);
    return TRUE;
}
