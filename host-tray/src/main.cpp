#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbt.h>
#include <shellapi.h>

#include "../resource.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT_PTR kProcessTimer = 1;
    constexpr UINT kExitCommand = 1001;
    constexpr UINT kAutoStartCommand = 1002;
    constexpr wchar_t kWindowClass[] = L"TabletDisplayTrayWindow";
    constexpr wchar_t kMutexName[] = L"Local\\TabletDisplayTray-48B668E5-4AE8-4A35-A64F-328827B7ABDA";
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValueName[] = L"TabletDisplay";
    constexpr GUID kSessionDisplayStatus{
        0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 }
    };

    NOTIFYICONDATAW g_trayIcon{};
    PROCESS_INFORMATION g_hostProcess{};
    HANDLE g_instanceMutex{};
    HPOWERNOTIFY g_displayPowerNotification{};
    bool g_exiting{};
    bool g_displayPoweredOn{ true };
    UINT g_taskbarCreated{};

    std::filesystem::path ExecutablePath()
    {
        std::vector<wchar_t> buffer(32'768);
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) return {};
        return std::filesystem::path(std::wstring(buffer.data(), length));
    }

    std::filesystem::path ExecutableDirectory()
    {
        return ExecutablePath().parent_path();
    }

    bool IsUkrainianUi() noexcept
    {
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_UKRAINIAN;
    }

    std::wstring AutoStartCommand()
    {
        const auto path = ExecutablePath();
        return path.empty() ? std::wstring{} : L"\"" + path.wstring() + L"\"";
    }

    bool IsAutoStartEnabled()
    {
        std::vector<wchar_t> value(32'768);
        DWORD valueBytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        const auto result = RegGetValueW(
            HKEY_CURRENT_USER,
            kRunKey,
            kRunValueName,
            RRF_RT_REG_SZ,
            nullptr,
            value.data(),
            &valueBytes);
        if (result != ERROR_SUCCESS) return false;
        const auto expected = AutoStartCommand();
        return !expected.empty() && _wcsicmp(value.data(), expected.c_str()) == 0;
    }

    bool SetAutoStartEnabled(bool enabled)
    {
        HKEY key{};
        const auto openResult = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRunKey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr);
        if (openResult != ERROR_SUCCESS) return false;

        LONG result{};
        if (enabled)
        {
            const auto command = AutoStartCommand();
            if (command.empty())
            {
                RegCloseKey(key);
                return false;
            }
            result = RegSetValueExW(
                key,
                kRunValueName,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            result = RegDeleteValueW(key, kRunValueName);
            if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
        }
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    void ReleaseTabletMouseButton() noexcept
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        (void)SendInput(1, &input, sizeof(input));
    }

    void CloseHostHandles() noexcept
    {
        if (g_hostProcess.hThread)
        {
            CloseHandle(g_hostProcess.hThread);
            g_hostProcess.hThread = nullptr;
        }
        if (g_hostProcess.hProcess)
        {
            CloseHandle(g_hostProcess.hProcess);
            g_hostProcess.hProcess = nullptr;
        }
        g_hostProcess.dwProcessId = 0;
        g_hostProcess.dwThreadId = 0;
    }

    void StopHost() noexcept
    {
        if (g_hostProcess.hProcess)
        {
            if (WaitForSingleObject(g_hostProcess.hProcess, 0) == WAIT_TIMEOUT)
            {
                (void)TerminateProcess(g_hostProcess.hProcess, 0);
                (void)WaitForSingleObject(g_hostProcess.hProcess, 5'000);
            }
            CloseHostHandles();
        }
        ReleaseTabletMouseButton();
    }

    bool StartHost(HWND owner)
    {
        if (g_hostProcess.hProcess && WaitForSingleObject(g_hostProcess.hProcess, 0) == WAIT_TIMEOUT)
            return true;
        CloseHostHandles();

        const auto directory = ExecutableDirectory();
        const auto hostPath = directory / L"TabletDisplayHost.exe";
        if (!std::filesystem::is_regular_file(hostPath))
        {
            MessageBoxW(
                owner,
                L"TabletDisplayHost.exe не знайдено.\n\nTabletDisplayHost.exe was not found.",
                L"TabletDisplay",
                MB_OK | MB_ICONERROR);
            return false;
        }

        const auto bundledAdb = directory / L"platform-tools" / L"adb.exe";
        if (std::filesystem::is_regular_file(bundledAdb))
            (void)SetEnvironmentVariableW(L"TABLETDISPLAY_ADB", bundledAdb.c_str());

        std::wstring command = L"\"" + hostPath.wstring() +
            L"\" --virtual-stream --reconnect --seconds 3600";
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        if (!CreateProcessW(
                hostPath.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                directory.c_str(),
                &startup,
                &g_hostProcess))
        {
            return false;
        }
        CloseHandle(g_hostProcess.hThread);
        g_hostProcess.hThread = nullptr;
        return true;
    }

    void AddTrayIcon(HWND window)
    {
        g_trayIcon = {};
        g_trayIcon.cbSize = sizeof(g_trayIcon);
        g_trayIcon.hWnd = window;
        g_trayIcon.uID = 1;
        g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        g_trayIcon.uCallbackMessage = kTrayMessage;
        g_trayIcon.hIcon = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        wcscpy_s(g_trayIcon.szTip, L"TabletDisplay");
        (void)Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
        g_trayIcon.uVersion = NOTIFYICON_VERSION_4;
        (void)Shell_NotifyIconW(NIM_SETVERSION, &g_trayIcon);
    }

    void ShowTrayMenu(HWND window)
    {
        const auto menu = CreatePopupMenu();
        if (!menu) return;
        const auto ukrainian = IsUkrainianUi();
        const auto autoStartText = ukrainian ? L"\u0410\u0432\u0442\u043e\u0437\u0430\u043f\u0443\u0441\u043a \u0456\u0437 Windows" : L"Start with Windows";
        const auto exitText = ukrainian ? L"\u0412\u0438\u0445\u0456\u0434" : L"Exit";
        const auto autoStartFlags = MF_STRING |
            (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED);
        (void)AppendMenuW(menu, autoStartFlags, kAutoStartCommand, autoStartText);
        (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        (void)AppendMenuW(menu, MF_STRING, kExitCommand, exitText);
        POINT point{};
        (void)GetCursorPos(&point);
        (void)SetForegroundWindow(window);
        (void)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
            point.x, point.y, 0, window, nullptr);
        (void)DestroyMenu(menu);
        (void)PostMessageW(window, WM_NULL, 0, 0);
    }

    LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == g_taskbarCreated)
        {
            AddTrayIcon(window);
            return 0;
        }
        switch (message)
        {
        case WM_CREATE:
            g_displayPowerNotification = RegisterPowerSettingNotification(
                window,
                &kSessionDisplayStatus,
                DEVICE_NOTIFY_WINDOW_HANDLE);
            AddTrayIcon(window);
            (void)SetTimer(window, kProcessTimer, 2'000, nullptr);
            (void)StartHost(window);
            return 0;
        case WM_TIMER:
            if (wParam == kProcessTimer && !g_exiting && g_displayPoweredOn)
            {
                if (!g_hostProcess.hProcess ||
                    WaitForSingleObject(g_hostProcess.hProcess, 0) == WAIT_OBJECT_0)
                {
                    CloseHostHandles();
                    (void)StartHost(window);
                }
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_POWERSETTINGCHANGE && lParam != 0)
            {
                const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                if (IsEqualGUID(setting->PowerSetting, kSessionDisplayStatus) &&
                    setting->DataLength >= sizeof(DWORD))
                {
                    DWORD displayState{};
                    std::memcpy(&displayState, setting->Data, sizeof(displayState));
                    const auto poweredOn = displayState != 0;
                    if (poweredOn != g_displayPoweredOn)
                    {
                        g_displayPoweredOn = poweredOn;
                        if (poweredOn)
                            (void)StartHost(window);
                        else
                            StopHost();
                    }
                }
                return TRUE;
            }
            break;
        case kTrayMessage:
            if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_RBUTTONUP ||
                LOWORD(lParam) == WM_CONTEXTMENU)
            {
                ShowTrayMenu(window);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kAutoStartCommand)
            {
                const auto enable = !IsAutoStartEnabled();
                if (!SetAutoStartEnabled(enable))
                {
                    MessageBoxW(
                        window,
                        IsUkrainianUi()
                            ? L"\u041d\u0435 \u0432\u0434\u0430\u043b\u043e\u0441\u044f \u0437\u043c\u0456\u043d\u0438\u0442\u0438 \u043d\u0430\u043b\u0430\u0448\u0442\u0443\u0432\u0430\u043d\u043d\u044f \u0430\u0432\u0442\u043e\u0437\u0430\u043f\u0443\u0441\u043a\u0443."
                            : L"Could not change the autostart setting.",
                        L"TabletDisplay",
                        MB_OK | MB_ICONERROR);
                }
                return 0;
            }
            if (LOWORD(wParam) == kExitCommand)
            {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_exiting = true;
            (void)KillTimer(window, kProcessTimer);
            if (g_displayPowerNotification)
            {
                (void)UnregisterPowerSettingNotification(g_displayPowerNotification);
                g_displayPowerNotification = nullptr;
            }
            (void)Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
            if (g_trayIcon.hIcon) DestroyIcon(g_trayIcon.hIcon);
            StopHost();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    g_instanceMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (g_instanceMutex) CloseHandle(g_instanceMutex);
        return 0;
    }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) return 1;

    const auto window = CreateWindowExW(
        0, kWindowClass, L"TabletDisplay", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CloseHandle(g_instanceMutex);
    return static_cast<int>(message.wParam);
}
