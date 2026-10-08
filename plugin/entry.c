/* The only translation unit coupled to the System Informer SDK. Keeping this
 * bridge in C lets the UI and transport use ordinary C++ and Win32 headers. */
#include <phdk.h>
#include <settings.h>
#include <toolstatusintf.h>
#include "host_bridge.h"

extern HWND WslCreateView(HWND parent, HINSTANCE instance);
extern void WslSetActive(BOOL active);
extern void WslShutdown(void);
extern void WslFocusContent(BOOL select);
extern void WslSearchChanged(void);
extern void WslHostRefreshChanged(BOOL automatic);
extern void WslHostRefresh(void);

static HINSTANCE PluginModule;
static PH_CALLBACK_REGISTRATION MainWindowRegistration;
static PH_CALLBACK_REGISTRATION UnloadRegistration;
static HWND ViewWindow;
static HWND HostWindow;
static PTOOLSTATUS_INTERFACE ToolStatus;
static PH_CALLBACK_REGISTRATION SearchChangedRegistration;
static BOOLEAN SearchCallbackRegistered;
static PH_STRINGREF SearchBanner = PH_STRINGREF_INIT(L"Search WSL");

DWORD WslHostIntegerSetting(PCWSTR name)
{
    return PhGetIntegerSetting(name);
}

DWORD WslHostRefreshInterval(void)
{
    DWORD interval = PhGetIntegerSetting(L"UpdateInterval");
    return interval ? interval : 1000;
}

BOOL WslHostRefreshAutomatically(void)
{
    return SystemInformer_GetUpdateAutomatically();
}

/* The SDK notifies tabs about automatic updates, but has no tab refresh event.
 * Observe the native View > Refresh command without consuming it, so both F5
 * and the menu update WSL even while the host's automatic updates are paused. */
static LRESULT CALLBACK HostWindowSubclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                           UINT_PTR subclassId, DWORD_PTR context)
{
    UNREFERENCED_PARAMETER(context);
    /* ID_VIEW_REFRESH from the host's public command resources. */
    const UINT refreshCommand = 10098;
    if (message == WM_COMMAND && LOWORD(wparam) == refreshCommand && ViewWindow)
        WslHostRefresh();
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, HostWindowSubclass, subclassId);
        HostWindow = NULL;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void WslApplyTheme(HWND window)
{
    PhInitializeWindowTheme(window, !!PhGetIntegerSetting(L"EnableThemeSupport"));
}

COLORREF WslDialogBackground(void)
{
    if (PhGetIntegerSetting(L"EnableThemeSupport"))
        return PhGetWindowThemePalette()->BackgroundColor;
    return GetSysColor(COLOR_3DFACE);
}

COLORREF WslDialogText(void)
{
    if (PhGetIntegerSetting(L"EnableThemeSupport"))
        return PhGetWindowThemePalette()->TextColor;
    return GetSysColor(COLOR_WINDOWTEXT);
}

BOOL WslIsDarkTheme(void)
{
    COLORREF background = WslDialogBackground();
    // The host's custom theme support also includes light palettes.
    ULONG luminance = 299 * GetRValue(background) + 587 * GetGValue(background) + 114 * GetBValue(background);
    return luminance < 128000;
}

HFONT WslGetHostFont(void)
{
    return SystemInformer_GetFont();
}

/* These factories return owned font handles. The main grid retains the host's
 * borrowed font; normal controls and text panes have separate font roles. */
HFONT WslCreateUiFont(HWND window)
{
    LONG dpi = (LONG)GetDpiForWindow(window);
    // PhCreateApplicationFont returns a reference-counted font wrapper in
    // this SDK, not a GDI HFONT. Win32 controls need the raw handle factory.
    HFONT result = PhCreateFontHandle(L"Microsoft Sans Serif", 8, FW_NORMAL, DEFAULT_PITCH, dpi ? dpi : 96);
    HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!result || result == stock)
    {
        LOGFONTW font;
        if (GetObjectW(stock, sizeof(font), &font))
            return CreateFontIndirectW(&font);
        return NULL;
    }
    return result;
}

HFONT WslCreateTextFont(HWND window)
{
    LONG dpi = (LONG)GetDpiForWindow(window);
    LOGFONTW font = {0};
    HFONT result = NULL;
    PPH_STRING setting = PhGetStringSetting(L"FontMonospace");

    // Match the host's serialized LOGFONT format, including a custom face and
    // size. A length check is required before decoding into the fixed struct.
    if (setting->Length == sizeof(font) * 2 * sizeof(WCHAR) &&
        PhHexStringToBuffer(&setting->sr, (PUCHAR)&font))
    {
        font.lfFaceName[LF_FACESIZE - 1] = UNICODE_NULL;
        result = CreateFontIndirectW(&font);
    }
    PhDereferenceObject(setting);
    if (result)
        return result;

    RtlZeroMemory(&font, sizeof(font));
    font.lfHeight = -MulDiv(9, dpi ? dpi : 96, 72);
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    font.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy_s(font.lfFaceName, RTL_NUMBER_OF(font.lfFaceName), L"Consolas");
    return CreateFontIndirectW(&font);
}

BOOL WslHasGlobalSearch(void)
{
    // ToolStatus.Config bit 1 is its public SearchBoxEnabled setting.
    return ToolStatus != NULL && (PhGetIntegerSetting(L"ToolStatus.Config") & 2) != 0;
}

BOOL WslMatchesGlobalSearch(PCWSTR text)
{
    PH_STRINGREF value;
    if (!ToolStatus || !ToolStatus->GetSearchMatchHandle())
        return TRUE;
    PhInitializeStringRef(&value, text ? text : L"");
    return ToolStatus->WordMatch(&value);
}

void WslClearGlobalSearch(void)
{
    HWND search, rebar;
    HWND mainWindow = ViewWindow ? GetAncestor(ViewWindow, GA_ROOT) : NULL;
    if (!mainWindow)
        return;
    if (!ToolStatus || !ToolStatus->GetSearchMatchHandle())
        return;
    // ToolStatus v2 exposes matching but no text setter. Its native search edit
    // is a direct child of the main window (or its rebar when reparented).
    search = FindWindowExW(mainWindow, NULL, WC_EDIT, NULL);
    if (!search)
    {
        rebar = FindWindowExW(mainWindow, NULL, REBARCLASSNAME, NULL);
        if (rebar)
            search = FindWindowExW(rebar, NULL, WC_EDIT, NULL);
    }
    if (search)
        SetWindowTextW(search, L"");
}

static VOID NTAPI SearchChanged(PVOID parameter, PVOID context)
{
    UNREFERENCED_PARAMETER(parameter);
    UNREFERENCED_PARAMETER(context);
    // ToolStatus raises this event synchronously on the main window thread.
    WslSearchChanged();
}

static VOID NTAPI ActivateContent(BOOLEAN select)
{
    WslFocusContent(select);
}

static HWND NTAPI GetTreeNewHandle(VOID)
{
    // ToolStatus calls this callback unconditionally for registered tabs.
    // Returning NULL opts out of TreeNew-only selection restoration safely.
    return NULL;
}

static BOOLEAN TabCallback(PPH_MAIN_TAB_PAGE page, PH_MAIN_TAB_PAGE_MESSAGE message, PVOID parameter1,
                           PVOID parameter2)
{
    UNREFERENCED_PARAMETER(page);
    switch (message)
    {
    case MainTabPageCreateWindow:
        ViewWindow = WslCreateView((HWND)parameter2, PluginModule);
        if (parameter1)
            *(HWND *)parameter1 = ViewWindow;
        return ViewWindow != NULL;
    case MainTabPageSelected:
        WslSetActive(parameter1 != NULL);
        return TRUE;
    case MainTabPageUpdateAutomaticallyChanged:
        WslHostRefreshChanged(parameter1 != NULL);
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
    PH_MAIN_TAB_PAGE page = {0};
    PPH_MAIN_TAB_PAGE addedPage;
    PTOOLSTATUS_TAB_INFO tabInfo;
    UNREFERENCED_PARAMETER(parameter);
    UNREFERENCED_PARAMETER(context);
    HostWindow = SystemInformer_GetWindowHandle();
    if (HostWindow)
        SetWindowSubclass(HostWindow, HostWindowSubclass, (UINT_PTR)HostWindowSubclass, 0);
    ToolStatus = PhGetPluginInterfaceZ(TOOLSTATUS_INTERFACE_NAME, TOOLSTATUS_INTERFACE_VERSION);
    if (ToolStatus && (!ToolStatus->GetSearchMatchHandle || !ToolStatus->WordMatch ||
                       !ToolStatus->RegisterTabInfo || !ToolStatus->SearchChangedEvent))
        ToolStatus = NULL;
    PhInitializeStringRef(&page.Name, L"WSL");
    page.Callback = TabCallback;
    addedPage = PhPluginCreateTabPage(&page);
    if (addedPage && ToolStatus)
    {
        tabInfo = ToolStatus->RegisterTabInfo(addedPage->Index, &SearchBanner);
        if (tabInfo)
        {
            tabInfo->ActivateContent = ActivateContent;
            tabInfo->GetTreeNewHandle = GetTreeNewHandle;
            PhRegisterCallback(ToolStatus->SearchChangedEvent, SearchChanged, NULL,
                               &SearchChangedRegistration);
            SearchCallbackRegistered = TRUE;
        }
        else
            ToolStatus = NULL;
    }
}

static VOID NTAPI PluginUnloading(PVOID parameter, PVOID context)
{
    UNREFERENCED_PARAMETER(parameter);
    UNREFERENCED_PARAMETER(context);
    if (SearchCallbackRegistered)
    {
        PhUnregisterCallback(ToolStatus->SearchChangedEvent, &SearchChangedRegistration);
        SearchCallbackRegistered = FALSE;
    }
    ToolStatus = NULL;
    if (HostWindow)
    {
        RemoveWindowSubclass(HostWindow, HostWindowSubclass, (UINT_PTR)HostWindowSubclass);
        HostWindow = NULL;
    }
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
    information->Description =
        L"Processes, connections, open files, modules and systemd services for WSL 2. MIT licensed.";
    information->HasOptions = FALSE;

    PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackMainWindowShowing), MainWindowShowing, NULL,
                       &MainWindowRegistration);
    PhRegisterCallback(PhGetPluginCallback(plugin, PluginCallbackUnload), PluginUnloading, NULL,
                       &UnloadRegistration);
    return TRUE;
}
