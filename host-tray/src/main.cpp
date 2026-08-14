#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "../resource.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT_PTR kProcessTimer = 1;
    constexpr UINT kExitCommand = 1001;
    constexpr wchar_t kWindowClass[] = L"TabletDisplayTrayWindow";
    constexpr wchar_t kMutexName[] = L"Local\\TabletDisplayTray-48B668E5-4AE8-4A35-A64F-328827B7ABDA";

    NOTIFYICONDATAW g_trayIcon{};
    PROCESS_INFORMATION g_hostProcess{};
    HANDLE g_instanceMutex{};
    bool g_exiting{};
    UINT g_taskbarCreated{};

    std::filesystem::path ExecutableDirectory()
    {
        std::vector<wchar_t> buffer(32'768);
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) return {};
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
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
        const auto exitText = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_UKRAINIAN
            ? L"Вихід"
            : L"Exit";
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
            AddTrayIcon(window);
            (void)SetTimer(window, kProcessTimer, 2'000, nullptr);
            (void)StartHost(window);
            return 0;
        case WM_TIMER:
            if (wParam == kProcessTimer && !g_exiting)
            {
                if (!g_hostProcess.hProcess ||
                    WaitForSingleObject(g_hostProcess.hProcess, 0) == WAIT_OBJECT_0)
                {
                    CloseHostHandles();
                    (void)StartHost(window);
                }
            }
            return 0;
        case kTrayMessage:
            if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_RBUTTONUP ||
                LOWORD(lParam) == WM_CONTEXTMENU)
            {
                ShowTrayMenu(window);
            }
            return 0;
        case WM_COMMAND:
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
