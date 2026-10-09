#pragma once
#include "capture_model.hpp"

namespace wsl
{
constexpr UINT CaptureChangedMessage = WM_APP + 93;

// The coordinator and models live on the host window thread. No tab window is
// needed to collect data; the selected view only supplies metadata preferences.
void startCapture(HINSTANCE module, HWND host);
void stopCapture();
std::vector<std::wstring> captureDistros();
std::shared_ptr<CaptureModel> captureModel(const std::wstring &distro);
void setCaptureView(HWND window, const std::wstring &distro, bool visible, Json snapshotRequest);
void detachCaptureView(HWND window);
void refreshCapture(const std::wstring &distro, bool reconnect = false);
void refreshCaptures(bool reconnect = false);
// Safe to call from a host Options callback on another window thread.
void captureSettingsChanged();
} // namespace wsl
