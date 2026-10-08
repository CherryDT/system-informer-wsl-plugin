#include "graphs.hpp"
#include "host_bridge.h"
#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace wsl::ui
{
namespace
{
constexpr int HistoryCapacity = 120;
struct GraphWindow
{
    View *view = nullptr;
    bool memory = false;
    HWND tooltip = nullptr;
    int mouseX = -1, hover = -1;
    std::wstring text;
};

double value(const GraphSample &sample, bool memory)
{
    return memory ? (sample.memoryTotal
                         ? 100.0 * (sample.memoryTotal - sample.memoryAvailable) / sample.memoryTotal
                         : 0)
                  : sample.cpu;
}

std::wstring exactBytes(uint64_t count)
{
    return bytes(count) + L" (" + std::to_wstring(count) + L" bytes)";
}

std::wstring describe(const GraphSample &sample, bool memory)
{
    FILETIME local{};
    SYSTEMTIME time{};
    FileTimeToLocalFileTime(&sample.timestamp, &local);
    FileTimeToSystemTime(&local, &time);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", time.wYear, time.wMonth, time.wDay, time.wHour,
               time.wMinute, time.wSecond, time.wMilliseconds);
    std::wstring text = stamp;
    if (sample.missing)
        return text + L"\nCapture was disabled.";
    if (memory)
    {
        text += L"\nVM memory used: " + number(value(sample, true), 3) + L"%\n";
        text += L"Used: " + exactBytes(sample.memoryTotal - sample.memoryAvailable);
        text += L"\nAvailable: " + exactBytes(sample.memoryAvailable);
        text += L"\nTotal: " + exactBytes(sample.memoryTotal);
        if (sample.largestRssPid)
            text += L"\nLargest process RSS: " + sample.largestRssName + L" (" +
                    std::to_wstring(sample.largestRssPid) + L") — " + exactBytes(sample.largestRss);
    }
    else
    {
        text += L"\nDistro CPU: " + number(sample.cpu, 3) + L"% of total WSL capacity";
        text += L"\n" + number(sample.cpu * sample.cpus, 3) + L"% of one vCPU; " +
                std::to_wstring(sample.cpus) + L" vCPUs";
        text += L"\nProcesses: " + std::to_wstring(sample.processCount);
    }
    if (sample.topPid)
        text += L"\nHighest CPU: " + sample.topName + L" (" + std::to_wstring(sample.topPid) + L") — " +
                number(sample.topCpu / sample.cpus, 3) + L"% of total (" + number(sample.topCpu, 3) +
                L"% of one vCPU)";
    else
        text += L"\nNo process CPU usage measured in this interval";
    return text + L"\nSample interval: " + number(sample.interval, 3) + L" s";
}

void hover(HWND window, GraphWindow &state)
{
    RECT rect{};
    GetClientRect(window, &rect);
    const auto &samples = state.view->graphSamples;
    int index = -1;
    if (state.mouseX >= 0 && rect.right > 2 && !samples.empty())
    {
        const int slot =
            static_cast<int>(std::lround((state.mouseX - 1) * (HistoryCapacity - 1.0) / (rect.right - 2)));
        index = slot - (HistoryCapacity - static_cast<int>(samples.size()));
        if (index < 0 || index >= static_cast<int>(samples.size()))
            index = -1;
    }
    state.hover = index;
    TOOLINFOW info{sizeof(info)};
    info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    info.hwnd = window;
    info.uId = 1;
    if (index >= 0)
    {
        state.text = describe(samples[index], state.memory);
        info.lpszText = state.text.data();
        SendMessageW(state.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
        POINT cursor{};
        if (!GetCursorPos(&cursor))
        {
            cursor = {state.mouseX, rect.bottom / 2};
            ClientToScreen(window, &cursor);
        }
        SendMessageW(state.tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(cursor.x + 16, cursor.y + 18));
    }
    SendMessageW(state.tooltip, TTM_TRACKACTIVATE, index >= 0, reinterpret_cast<LPARAM>(&info));
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK graphProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto state = reinterpret_cast<GraphWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        auto incoming = static_cast<std::unique_ptr<GraphWindow> *>(
            reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        state = incoming->release();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE: {
        state->tooltip = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
        TOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        info.hwnd = window;
        info.uId = 1;
        info.lpszText = const_cast<wchar_t *>(L"");
        SendMessageW(state->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        SendMessageW(state->tooltip, TTM_SETMAXTIPWIDTH, 0, scale(window, 620));
        SendMessageW(state->tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        WslApplyTheme(state->tooltip);
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        state->mouseX = GET_X_LPARAM(lparam);
        hover(window, *state);
        return 0;
    }
    case WM_MOUSELEAVE:
        state->mouseX = -1;
        hover(window, *state);
        return 0;
    case GraphSampleChanged:
    case WM_SIZE:
        hover(window, *state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(window, &paint);
        RECT r{};
        GetClientRect(window, &r);
        if (r.right <= 0 || r.bottom <= 0)
        {
            EndPaint(window, &paint);
            return 0;
        }
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, r.right, r.bottom);
        auto originalBitmap = SelectObject(dc, bitmap);
        const bool dark = WslIsDarkTheme() != FALSE;
        SetDCBrushColor(dc, dark ? WslDialogBackground() : GetSysColor(COLOR_WINDOW));
        FillRect(dc, &r, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        HPEN grid = CreatePen(PS_SOLID, 1, dark ? RGB(70, 70, 70) : GetSysColor(COLOR_3DFACE));
        auto originalPen = SelectObject(dc, grid);
        for (int i = 1; i < 4; ++i)
        {
            MoveToEx(dc, 1, r.bottom * i / 4, nullptr);
            LineTo(dc, r.right - 1, r.bottom * i / 4);
        }
        // Grid columns are tied to sample sequence, so they travel left with
        // their samples instead of staying fixed against the scrolling graph.
        const auto &v = *state->view;
        const auto &samples = v.graphSamples;
        for (int slot = 0; slot < HistoryCapacity; ++slot)
            if ((v.graphSequence + slot) % 10 == 0)
            {
                int x = 1 + slot * (r.right - 2) / (HistoryCapacity - 1);
                MoveToEx(dc, x, 1, nullptr);
                LineTo(dc, x, r.bottom - 1);
            }
        SelectObject(dc, originalPen);
        DeleteObject(grid);
        const COLORREF color = WslHostIntegerSetting(state->memory ? L"ColorPhysical" : L"ColorCpuUser");
        HPEN curve = CreatePen(PS_SOLID, 1, color);
        SelectObject(dc, curve);
        auto point = [&](size_t index) {
            return POINT{1 + (static_cast<int>(index) + HistoryCapacity - static_cast<int>(samples.size())) *
                                 (r.right - 2) / (HistoryCapacity - 1),
                         r.bottom - 2 -
                             static_cast<LONG>(std::clamp(value(samples[index], state->memory), 0.0, 100.0) *
                                               (r.bottom - 4) / 100.0)};
        };
        for (size_t i = 0; i < samples.size(); ++i)
        {
            if (samples[i].missing)
                continue;
            const auto p = point(i);
            if (i && !samples[i - 1].missing)
                LineTo(dc, p.x, p.y);
            else
                MoveToEx(dc, p.x, p.y, nullptr);
        }
        if (state->hover >= 0 && static_cast<size_t>(state->hover) < samples.size())
        {
            const auto p = point(state->hover);
            MoveToEx(dc, p.x, 1, nullptr);
            LineTo(dc, p.x, r.bottom - 1);
            if (!samples[state->hover].missing)
            {
                auto brush = CreateSolidBrush(color);
                auto oldBrush = SelectObject(dc, brush);
                Ellipse(dc, p.x - 3, p.y - 3, p.x + 4, p.y + 4);
                SelectObject(dc, oldBrush);
                DeleteObject(brush);
            }
        }
        SelectObject(dc, originalPen);
        DeleteObject(curve);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dark ? RGB(210, 210, 210) : GetSysColor(COLOR_GRAYTEXT));
        auto originalFont = SelectObject(dc, font);
        RECT label = r;
        InflateRect(&label, -scale(window, 5), -scale(window, 3));
        std::wstring caption = state->memory ? L"WSL VM memory used" : L"Distro CPU";
        if (!samples.empty() && !samples.back().missing)
        {
            caption += L"  " + number(value(samples.back(), state->memory)) + L"%";
            if (state->memory)
                caption += L" (" + bytes(samples.back().memoryTotal - samples.back().memoryAvailable) + L")";
        }
        DrawTextW(dc, caption.c_str(), -1, &label, DT_LEFT | DT_TOP | DT_SINGLELINE);
        SelectObject(dc, originalFont);
        SetDCBrushColor(dc, dark ? RGB(100, 100, 100) : GetSysColor(COLOR_3DSHADOW));
        FrameRect(dc, &r, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, originalBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCDESTROY:
        DestroyWindow(state->tooltip);
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

HWND createHistoryGraph(HWND parent, View &view, bool memory)
{
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = graphProc;
    cls.lpszClassName = L"WslTools.Graph";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    auto state = std::make_unique<GraphWindow>();
    state->view = &view;
    state->memory = memory;
    return CreateWindowExW(0, cls.lpszClassName, memory ? L"VM memory history" : L"CPU history",
                           WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr, instance, &state);
}
} // namespace wsl::ui
