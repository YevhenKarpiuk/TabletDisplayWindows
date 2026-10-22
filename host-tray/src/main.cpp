#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include "../resource.h"
#include "display/DisplayModes.h"
#include "wifi/WifiClient.h"
#include "wifi/WifiDiscovery.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT kDevicesMessage = WM_APP + 2;
    // wParam: bit mask of the monitors whose Hosts stopped; lParam: the stop worker id.
    constexpr UINT kHostsStoppedMessage = WM_APP + 3;
    constexpr UINT kExitCommand = 1001;
    // kPairCommandBase + N pairs the N-th unpaired Wi-Fi tablet found on the network;
    // kForgetCommandBase + N forgets the N-th paired one.
    constexpr UINT kPairCommandBase = 1200;
    constexpr UINT kForgetCommandBase = 1300;
    constexpr UINT kMaxWifiMenuItems = 16;
    constexpr UINT kPairingFinishedMessage = WM_APP + 4;
    constexpr DWORD kWifiPollMs = 5'000;
    // A Wi-Fi tablet missed by a few discovery rounds still counts as present.
    constexpr ULONGLONG kWifiPresenceMs = 20'000;
    constexpr wchar_t kWifiSerialPrefix[] = L"wifi-";
    constexpr UINT kAutoStartCommand = 1002;
    // kResolutionCommandBase + 10 * monitor index + N selects that tablet's resolution
    // divided by N (1..4).
    constexpr UINT kResolutionCommandBase = 1100;
    constexpr UINT kResolutionCommandStride = 10;
    // Within a monitor's command range: refresh rate choices and the decoder limit override.
    constexpr UINT kFps60CommandOffset = 5;
    constexpr UINT kFps30CommandOffset = 6;
    constexpr UINT kIgnoreDecoderLimitCommandOffset = 7;
    constexpr DWORD kMaxResolutionDivisor = 4;
    // One TabletDisplay monitor, Host process and adb forward port per connected tablet;
    // the driver offers up to four monitors.
    constexpr DWORD kMaxMonitors = 4;
    constexpr DWORD kBasePort = 27183;
    constexpr DWORD kDevicePollMs = 2'000;
    constexpr ULONGLONG kAdbTimeoutMs = 10'000;
    constexpr wchar_t kWindowClass[] = L"TabletDisplayTrayWindow";
    constexpr wchar_t kMutexName[] = L"Local\\TabletDisplayTray-48B668E5-4AE8-4A35-A64F-328827B7ABDA";
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValueName[] = L"TabletDisplay";
    constexpr wchar_t kSettingsKey[] = L"Software\\TabletDisplay";
    constexpr wchar_t kResolutionDivisorValueName[] = L"ResolutionDivisor";
    constexpr wchar_t kMonitorsKey[] = L"Software\\TabletDisplay\\Monitors";
    // Per tablet: its resolution divisor, and the panel size and orientation its Host last saw.
    constexpr wchar_t kDevicesKey[] = L"Software\\TabletDisplay\\Devices\\";
    constexpr GUID kSessionDisplayStatus{
        0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 }
    };

    // A tablet the tray can stream to: an adb serial, or "wifi-<id>" for a paired Wi-Fi tablet.
    struct AdbDevice
    {
        std::wstring serial;
        std::wstring model;
        bool ready{};
        std::wstring address;
    };

    struct SeenWifiTablet
    {
        tabletdisplay::wifi::DiscoveredTablet tablet;
        ULONGLONG lastSeen{};
    };

    // The Host streaming one TabletDisplay monitor; serial is empty while the monitor is free.
    // A stopping slot keeps its serial and stays reserved until its Host has exited, so a new
    // Host never competes with the old one for the monitor lease or the adb forward port.
    struct HostSlot
    {
        std::wstring serial;
        PROCESS_INFORMATION process{};
        HANDLE stopEvent{};
        std::wstring stopEventName;
        std::wstring address;
        bool stopping{};
    };

    struct StoppingHost
    {
        DWORD index;
        HANDLE process;
    };

    NOTIFYICONDATAW g_trayIcon{};
    HostSlot g_hosts[kMaxMonitors];
    std::mutex g_devicesMutex;
    std::vector<AdbDevice> g_devices;
    std::jthread g_deviceWatcher;
    std::jthread g_wifiWatcher;
    std::jthread g_pairingWorker;
    std::atomic_bool g_pairingActive{};
    std::map<std::wstring, SeenWifiTablet> g_wifiTablets;
    // Snapshots behind the Wi-Fi menu items of the last opened tray menu.
    std::vector<tabletdisplay::wifi::DiscoveredTablet> g_pairCandidates;
    std::vector<tabletdisplay::wifi::PairedTablet> g_forgetCandidates;
    // Workers that wait for stopping Hosts, keyed by the id they post back to the window.
    std::map<LPARAM, std::jthread> g_stopWorkers;
    LPARAM g_nextStopWorker{};
    bool g_reportedMissingHost{};
    HANDLE g_instanceMutex{};
    HPOWERNOTIFY g_displayPowerNotification{};
    bool g_exiting{};
    bool g_displayPoweredOn{ true };
    bool g_sessionLocked{};
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

    DWORD ResolutionDivisor() noexcept
    {
        DWORD value{};
        DWORD valueBytes = sizeof(value);
        const auto result = RegGetValueW(
            HKEY_CURRENT_USER,
            kSettingsKey,
            kResolutionDivisorValueName,
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &valueBytes);
        return result == ERROR_SUCCESS && value >= 1 && value <= kMaxResolutionDivisor ? value : 1;
    }

    bool SetResolutionDivisor(DWORD divisor) noexcept
    {
        return RegSetKeyValueW(
            HKEY_CURRENT_USER,
            kSettingsKey,
            kResolutionDivisorValueName,
            REG_DWORD,
            &divisor,
            sizeof(divisor)) == ERROR_SUCCESS;
    }

    DWORD DeviceValue(const std::wstring& serial, const wchar_t* name, DWORD fallback) noexcept
    {
        DWORD value{};
        DWORD valueBytes = sizeof(value);
        const auto key = kDevicesKey + serial;
        const auto result = RegGetValueW(
            HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &valueBytes);
        return result == ERROR_SUCCESS ? value : fallback;
    }

    bool SetDeviceValue(const std::wstring& serial, const wchar_t* name, DWORD value) noexcept
    {
        const auto key = kDevicesKey + serial;
        return RegSetKeyValueW(
            HKEY_CURRENT_USER, key.c_str(), name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
    }

    // The virtual display refresh and stream rate chosen for the tablet: 60 or 30 Hz.
    DWORD DeviceFps(const std::wstring& serial) noexcept
    {
        return DeviceValue(serial, L"Fps", 60) == 30 ? 30 : 60;
    }

    // Streams the chosen resolution even when the tablet decoder reports it cannot play it.
    bool DeviceIgnoresDecoderLimit(const std::wstring& serial) noexcept
    {
        return DeviceValue(serial, L"IgnoreDecoderLimit", 0) != 0;
    }

    // Tablets without their own choice start with the resolution chosen before per-tablet settings.
    DWORD DeviceResolutionDivisor(const std::wstring& serial) noexcept
    {
        const auto value = DeviceValue(serial, kResolutionDivisorValueName, ResolutionDivisor());
        return value >= 1 && value <= kMaxResolutionDivisor ? value : 1;
    }

    bool SetDeviceResolutionDivisor(const std::wstring& serial, DWORD divisor) noexcept
    {
        const auto key = kDevicesKey + serial;
        return RegSetKeyValueW(
            HKEY_CURRENT_USER, key.c_str(), kResolutionDivisorValueName, REG_DWORD,
            &divisor, sizeof(divisor)) == ERROR_SUCCESS;
    }

    // The Windows monitor size for the divisor in the tablet's current orientation, or nothing
    // until the tablet's Host has connected once.
    std::optional<tabletdisplay::display::Mode> DeviceMode(const std::wstring& serial, DWORD divisor) noexcept
    {
        const auto mode = tabletdisplay::display::ScaledMode(
            DeviceValue(serial, L"NativeWidth", 0), DeviceValue(serial, L"NativeHeight", 0), divisor);
        if (!tabletdisplay::display::IsSupportedMode(mode)) return std::nullopt;
        if (DeviceValue(serial, L"Portrait", 0) != 0) return tabletdisplay::display::Mode{ mode.height, mode.width };
        return mode;
    }

    void ReleaseTabletMouseButton() noexcept
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        (void)SendInput(1, &input, sizeof(input));
    }

    void RestoreTaskbarWindowRegistration() noexcept
    {
        (void)EnumWindows(
            [](HWND window, LPARAM) -> BOOL
            {
                if (IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr)
                    (void)ShowWindowAsync(window, SW_SHOWNA);
                return TRUE;
            },
            0);
    }

    std::wstring EnvironmentValue(const wchar_t* name)
    {
        const auto size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size == 0) return {};
        std::wstring value(size, L'\0');
        value.resize(GetEnvironmentVariableW(name, value.data(), size));
        return value;
    }

    // The same search order as TabletDisplayHost, starting with the bundled platform-tools.
    std::filesystem::path FindAdb()
    {
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(ExecutableDirectory() / L"platform-tools" / L"adb.exe");
        const auto explicitPath = EnvironmentValue(L"TABLETDISPLAY_ADB");
        if (!explicitPath.empty()) candidates.emplace_back(explicitPath);
        const auto androidHome = EnvironmentValue(L"ANDROID_HOME");
        if (!androidHome.empty())
            candidates.push_back(std::filesystem::path(androidHome) / L"platform-tools" / L"adb.exe");
        candidates.emplace_back(L"C:\\tools\\android\\sdk\\platform-tools\\adb.exe");
        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr) != 0)
            candidates.emplace_back(found);
        for (const auto& candidate : candidates)
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) return candidate;
        }
        return {};
    }

    // Runs adb with the arguments and returns its output, or nothing when adb fails or hangs.
    std::optional<std::string> AdbOutput(const std::filesystem::path& adb, const std::wstring& arguments)
    {
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE readPipe{};
        HANDLE writePipe{};
        if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return std::nullopt;
        if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0))
        {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return std::nullopt;
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        PROCESS_INFORMATION process{};
        auto command = L"\"" + adb.wstring() + L"\" " + arguments;
        const auto created = CreateProcessW(
            adb.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(writePipe);
        if (!created)
        {
            CloseHandle(readPipe);
            return std::nullopt;
        }

        // The adb server started by the first command inherits the pipe and keeps it open,
        // so the output is read only while the adb client process itself is running.
        std::string output;
        char buffer[4096];
        const auto drain = [&]
        {
            DWORD available{};
            while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available != 0)
            {
                DWORD count{};
                if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) ||
                    count == 0)
                    return;
                output.append(buffer, count);
            }
        };
        const auto deadline = GetTickCount64() + kAdbTimeoutMs;
        bool finished{};
        while (!finished)
        {
            finished = WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0;
            drain();
            if (!finished && GetTickCount64() >= deadline) break;
        }
        if (!finished)
        {
            (void)TerminateProcess(process.hProcess, 1);
            (void)WaitForSingleObject(process.hProcess, 1'000);
        }
        DWORD exitCode{ 1 };
        (void)GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readPipe);
        if (!finished || exitCode != 0) return std::nullopt;
        return output;
    }

    std::vector<AdbDevice> ParseAdbDevices(const std::string& output)
    {
        std::vector<AdbDevice> devices;
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.starts_with("List of devices") || line.front() == '*') continue;
            std::istringstream fields(line);
            std::string serial;
            std::string state;
            fields >> serial >> state;
            if (serial.empty() || state.empty()) continue;
            std::string model;
            for (std::string field; fields >> field;)
            {
                if (field.starts_with("model:")) model = field.substr(6);
            }
            std::replace(model.begin(), model.end(), '_', ' ');
            devices.push_back({
                std::wstring(serial.begin(), serial.end()),
                std::wstring(model.begin(), model.end()),
                state == "device" });
        }
        return devices;
    }

    // Polls adb on a worker thread, because adb can block for seconds, and posts the device
    // list to the tray window.
    void WatchDevices(std::stop_token stopToken, HWND window)
    {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        // Wi-Fi address of each USB tablet, refreshed every 30 s: a tablet that is both plugged in
        // and found on the network streams over USB only.
        std::map<std::wstring, std::pair<std::wstring, ULONGLONG>> wifiAddresses;
        do
        {
            const auto adb = FindAdb();
            const auto output = adb.empty() ? std::nullopt : AdbOutput(adb, L"devices -l");
            if (output)
            {
                auto devices = ParseAdbDevices(*output);
                const auto now = GetTickCount64();
                for (auto& device : devices)
                {
                    if (!device.ready) continue;
                    auto& cached = wifiAddresses[device.serial];
                    if (cached.second == 0 || now - cached.second > 30'000)
                    {
                        cached = { {}, now };
                        const auto ip = AdbOutput(adb, L"-s \"" + device.serial + L"\" shell ip -4 addr show wlan0");
                        const auto inet = ip ? ip->find("inet ") : std::string::npos;
                        if (inet != std::string::npos)
                        {
                            const auto start = inet + 5;
                            const auto end = ip->find_first_of("/ \r\n", start);
                            const auto text = ip->substr(start, end == std::string::npos ? std::string::npos : end - start);
                            cached.first.assign(text.begin(), text.end());
                        }
                    }
                    device.address = cached.first;
                }
                {
                    std::lock_guard devicesLock(g_devicesMutex);
                    g_devices = std::move(devices);
                }
                (void)PostMessageW(window, kDevicesMessage, 0, 0);
            }
        } while (!wake.wait_for(lock, stopToken, std::chrono::milliseconds(kDevicePollMs), [] { return false; }));
    }

    // Browses the network for TabletDisplay apps (DNS-SD) and posts the result to the window.
    void WatchWifiTablets(std::stop_token stopToken, HWND window)
    {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        do
        {
            const auto found = tabletdisplay::wifi::DiscoverTablets(1'500);
            const auto now = GetTickCount64();
            {
                std::lock_guard devicesLock(g_devicesMutex);
                for (const auto& tablet : found) g_wifiTablets[tablet.id] = { tablet, now };
                std::erase_if(g_wifiTablets, [now](const auto& entry)
                {
                    return now - entry.second.lastSeen > kWifiPresenceMs;
                });
            }
            (void)PostMessageW(window, kDevicesMessage, 0, 0);
        } while (!wake.wait_for(lock, stopToken, std::chrono::milliseconds(kWifiPollMs), [] { return false; }));
    }

    bool IsWifiSerial(const std::wstring& serial)
    {
        return serial.starts_with(kWifiSerialPrefix);
    }

    // USB tablets from adb plus the paired tablets currently found on the network.
    std::vector<AdbDevice> CombinedDevices()
    {
        std::vector<AdbDevice> devices;
        std::vector<SeenWifiTablet> wifiTablets;
        {
            std::lock_guard lock(g_devicesMutex);
            devices = g_devices;
            for (const auto& [id, seen] : g_wifiTablets) wifiTablets.push_back(seen);
        }
        std::set<std::wstring> paired;
        for (const auto& tablet : tabletdisplay::wifi::LoadPairings())
            paired.insert(tabletdisplay::wifi::ToHex(tablet.id));
        // A tablet plugged in over USB is streamed over USB, not a second time over Wi-Fi.
        std::set<std::wstring> usbAddresses;
        for (const auto& device : devices)
        {
            if (device.ready && !device.address.empty()) usbAddresses.insert(device.address);
        }
        for (const auto& seen : wifiTablets)
        {
            if (!paired.contains(seen.tablet.id) || usbAddresses.contains(seen.tablet.address)) continue;
            devices.push_back({
                kWifiSerialPrefix + seen.tablet.id,
                seen.tablet.name + L" (Wi-Fi)",
                true,
                seen.tablet.address });
        }
        return devices;
    }

    // Each tablet keeps the monitor it got first, so Windows restores its display position.
    DWORD SavedMonitor(const std::wstring& serial) noexcept
    {
        DWORD value{};
        DWORD valueBytes = sizeof(value);
        const auto result = RegGetValueW(
            HKEY_CURRENT_USER, kMonitorsKey, serial.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &valueBytes);
        return result == ERROR_SUCCESS && value >= 1 && value <= kMaxMonitors ? value : 0;
    }

    void SaveMonitor(const std::wstring& serial, DWORD monitor) noexcept
    {
        (void)RegSetKeyValueW(
            HKEY_CURRENT_USER, kMonitorsKey, serial.c_str(), REG_DWORD, &monitor, sizeof(monitor));
    }

    bool MonitorSavedForOtherTablet(DWORD monitor, const std::wstring& serial)
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kMonitorsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return false;
        bool saved{};
        for (DWORD index = 0; !saved; ++index)
        {
            wchar_t name[256]{};
            DWORD nameChars = ARRAYSIZE(name);
            DWORD type{};
            DWORD value{};
            DWORD valueBytes = sizeof(value);
            const auto result = RegEnumValueW(
                key, index, name, &nameChars, nullptr, &type, reinterpret_cast<BYTE*>(&value), &valueBytes);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result != ERROR_SUCCESS) continue;
            saved = type == REG_DWORD && value == monitor && serial != name;
        }
        RegCloseKey(key);
        return saved;
    }

    // Returns the 1-based monitor for the tablet, or 0 when all monitors are busy.
    DWORD AssignMonitor(const std::wstring& serial)
    {
        const auto isFree = [](DWORD monitor) { return g_hosts[monitor - 1].serial.empty(); };
        const auto saved = SavedMonitor(serial);
        if (saved != 0 && isFree(saved)) return saved;
        DWORD fallback{};
        for (DWORD monitor = 1; monitor <= kMaxMonitors; ++monitor)
        {
            if (!isFree(monitor)) continue;
            if (!MonitorSavedForOtherTablet(monitor, serial))
            {
                SaveMonitor(serial, monitor);
                return monitor;
            }
            if (fallback == 0) fallback = monitor;
        }
        if (fallback != 0) SaveMonitor(serial, fallback);
        return fallback;
    }

    void CloseHostHandles(HostSlot& host) noexcept
    {
        if (host.process.hThread)
        {
            CloseHandle(host.process.hThread);
            host.process.hThread = nullptr;
        }
        if (host.process.hProcess)
        {
            CloseHandle(host.process.hProcess);
            host.process.hProcess = nullptr;
        }
        host.process.dwProcessId = 0;
        host.process.dwThreadId = 0;
    }

    bool IsHostRunning(const HostSlot& host) noexcept
    {
        return host.process.hProcess && WaitForSingleObject(host.process.hProcess, 0) == WAIT_TIMEOUT;
    }

    // Signals the selected Hosts to stop and takes their process handles; the slots stay
    // reserved (stopping) until FinishStoppingHosts has waited for the processes.
    std::vector<StoppingHost> BeginStoppingHosts(const std::array<bool, kMaxMonitors>& selected)
    {
        std::vector<StoppingHost> stopping;
        for (DWORD index = 0; index < kMaxMonitors; ++index)
        {
            auto& host = g_hosts[index];
            if (!selected[index]) continue;
            // A graceful stop lets the Host end the stream and detach the monitor itself.
            if (IsHostRunning(host) && host.stopEvent) (void)SetEvent(host.stopEvent);
            if (host.process.hProcess)
            {
                stopping.push_back({ index, host.process.hProcess });
                host.process.hProcess = nullptr;
            }
            CloseHostHandles(host);
            host.stopping = !host.serial.empty();
        }
        return stopping;
    }

    // Waits for the Hosts together, so several tablets take one timeout, and terminates those
    // that do not exit in time. Runs off the window thread except when the tray exits.
    void FinishStoppingHosts(const std::vector<StoppingHost>& hosts) noexcept
    {
        bool terminatedRunningHost{};
        const auto deadline = GetTickCount64() + 4'000;
        for (const auto& host : hosts)
        {
            const auto now = GetTickCount64();
            const auto wait = static_cast<DWORD>(deadline > now ? deadline - now : 0);
            if (WaitForSingleObject(host.process, wait) == WAIT_TIMEOUT)
            {
                terminatedRunningHost = true;
                (void)TerminateProcess(host.process, 0);
                (void)WaitForSingleObject(host.process, 5'000);
            }
            CloseHandle(host.process);
        }
        if (terminatedRunningHost)
        {
            // The terminated Host could not release the button or migrate taskbar buttons.
            ReleaseTabletMouseButton();
            Sleep(100);
            RestoreTaskbarWindowRegistration();
            Sleep(250);
            RestoreTaskbarWindowRegistration();
        }
    }

    // Stops the selected Hosts without blocking the tray; kHostsStoppedMessage frees their
    // monitors afterwards.
    void StopHosts(HWND window, const std::array<bool, kMaxMonitors>& selected)
    {
        WPARAM mask{};
        for (DWORD index = 0; index < kMaxMonitors; ++index)
        {
            if (selected[index] && !g_hosts[index].stopping) mask |= WPARAM{ 1 } << index;
        }
        if (mask == 0) return;
        std::array<bool, kMaxMonitors> starting{};
        for (DWORD index = 0; index < kMaxMonitors; ++index)
            starting[index] = (mask & (WPARAM{ 1 } << index)) != 0;
        auto hosts = BeginStoppingHosts(starting);
        const auto id = ++g_nextStopWorker;
        g_stopWorkers.emplace(id, std::jthread([window, mask, id, hosts = std::move(hosts)]
        {
            FinishStoppingHosts(hosts);
            (void)PostMessageW(window, kHostsStoppedMessage, mask, id);
        }));
    }

    void StopAllHosts(HWND window)
    {
        std::array<bool, kMaxMonitors> all{};
        all.fill(true);
        StopHosts(window, all);
    }

    void HostsStopped(WPARAM mask, LPARAM id)
    {
        const auto worker = g_stopWorkers.find(id);
        if (worker != g_stopWorkers.end())
        {
            if (worker->second.joinable()) worker->second.join();
            g_stopWorkers.erase(worker);
        }
        for (DWORD index = 0; index < kMaxMonitors; ++index)
        {
            if ((mask & (WPARAM{ 1 } << index)) == 0) continue;
            g_hosts[index].stopping = false;
            g_hosts[index].serial.clear();
        }
    }

    // Used when the tray exits: stops every Host before the process ends.
    void StopAllHostsNow()
    {
        std::array<bool, kMaxMonitors> all{};
        all.fill(true);
        FinishStoppingHosts(BeginStoppingHosts(all));
        for (auto& [id, worker] : g_stopWorkers)
        {
            if (worker.joinable()) worker.join();
        }
        g_stopWorkers.clear();
        for (auto& host : g_hosts)
        {
            host.stopping = false;
            host.serial.clear();
        }
    }

    bool StartHost(HWND owner, DWORD monitor, const std::wstring& serial, const std::wstring& address)
    {
        auto& host = g_hosts[monitor - 1];
        CloseHostHandles(host);

        const auto directory = ExecutableDirectory();
        const auto hostPath = directory / L"TabletDisplayHost.exe";
        if (!std::filesystem::is_regular_file(hostPath))
        {
            if (!g_reportedMissingHost)
            {
                g_reportedMissingHost = true;
                MessageBoxW(
                    owner,
                    L"TabletDisplayHost.exe не знайдено.\n\nTabletDisplayHost.exe was not found.",
                    L"TabletDisplay",
                    MB_OK | MB_ICONERROR);
            }
            return false;
        }

        const auto bundledAdb = directory / L"platform-tools" / L"adb.exe";
        if (std::filesystem::is_regular_file(bundledAdb))
            (void)SetEnvironmentVariableW(L"TABLETDISPLAY_ADB", bundledAdb.c_str());

        if (!host.stopEvent)
        {
            host.stopEventName = L"Local\\TabletDisplayHostStop-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(monitor);
            host.stopEvent = CreateEventW(nullptr, TRUE, FALSE, host.stopEventName.c_str());
            if (!host.stopEvent) return false;
        }
        (void)ResetEvent(host.stopEvent);

        // Every tablet gets its own Host and monitor; USB tablets also their own adb forward port.
        const auto transport = IsWifiSerial(serial)
            ? L" --wifi-tablet " + serial.substr(wcslen(kWifiSerialPrefix)) +
                (address.empty() ? std::wstring() : L" --wifi-address " + address)
            : L" --serial \"" + serial + L"\" --port " + std::to_wstring(kBasePort + monitor - 1);
        std::wstring command = L"\"" + hostPath.wstring() +
            L"\" --virtual-stream --reconnect --seconds 0" + transport +
            L" --monitor " + std::to_wstring(monitor) +
            L" --scale " + std::to_wstring(DeviceResolutionDivisor(serial)) +
            L" --fps " + std::to_wstring(DeviceFps(serial)) +
            (DeviceIgnoresDecoderLimit(serial) ? L" --ignore-decoder-limit" : L"") +
            L" --stop-event " + host.stopEventName;
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
                &host.process))
        {
            return false;
        }
        CloseHandle(host.process.hThread);
        host.process.hThread = nullptr;
        host.serial = serial;
        host.address = address;
        return true;
    }

    void UpdateTrayTip() noexcept;

    // Starts a Host for every authorized tablet, restarts Hosts that exited and stops the Hosts
    // of tablets that were unplugged.
    void ReconcileHosts(HWND window)
    {
        if (g_exiting || !g_displayPoweredOn || g_sessionLocked) return;
        const auto devices = CombinedDevices();
        const auto find = [&](const std::wstring& serial)
        {
            return std::find_if(devices.begin(), devices.end(), [&](const AdbDevice& device)
            {
                return device.serial == serial;
            });
        };

        std::array<bool, kMaxMonitors> unplugged{};
        bool anyUnplugged{};
        for (DWORD index = 0; index < kMaxMonitors; ++index)
        {
            const auto& host = g_hosts[index];
            if (!host.serial.empty() && !host.stopping && find(host.serial) == devices.end())
            {
                unplugged[index] = true;
                anyUnplugged = true;
            }
        }
        if (anyUnplugged) StopHosts(window, unplugged);

        for (DWORD index = 0; index < kMaxMonitors; ++index)
        {
            auto& host = g_hosts[index];
            if (host.serial.empty() || host.stopping || IsHostRunning(host)) continue;
            // The Host exited on its own (for example its watchdog fired): restart it.
            const auto serial = host.serial;
            const auto device = find(serial);
            (void)StartHost(window, index + 1, serial, device != devices.end() ? device->address : host.address);
        }

        for (const auto& device : devices)
        {
            if (!device.ready) continue;
            const bool hasHost = std::any_of(std::begin(g_hosts), std::end(g_hosts), [&](const HostSlot& host)
            {
                return host.serial == device.serial;
            });
            if (hasHost) continue;
            const auto monitor = AssignMonitor(device.serial);
            if (monitor != 0) (void)StartHost(window, monitor, device.serial, device.address);
        }
        UpdateTrayTip();
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

    void UpdateTrayTip() noexcept
    {
        const auto count = std::count_if(std::begin(g_hosts), std::end(g_hosts), [](const HostSlot& host)
        {
            return !host.serial.empty();
        });
        std::wstring tip = L"TabletDisplay";
        if (count > 0)
            tip += (IsUkrainianUi() ? L": \u043f\u043b\u0430\u043d\u0448\u0435\u0442\u0456\u0432 " : L": tablets ") +
                std::to_wstring(count);
        if (tip == g_trayIcon.szTip) return;
        wcsncpy_s(g_trayIcon.szTip, tip.c_str(), _TRUNCATE);
        g_trayIcon.uFlags = NIF_TIP | NIF_SHOWTIP;
        (void)Shell_NotifyIconW(NIM_MODIFY, &g_trayIcon);
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

        const wchar_t* const ukrainianLabels[] = {
            L"\u041e\u0440\u0438\u0433\u0456\u043d\u0430\u043b\u044c\u043d\u0430",
            L"\u0423 2 \u0440\u0430\u0437\u0438 \u043c\u0435\u043d\u0448\u0430",
            L"\u0423 3 \u0440\u0430\u0437\u0438 \u043c\u0435\u043d\u0448\u0430",
            L"\u0423 4 \u0440\u0430\u0437\u0438 \u043c\u0435\u043d\u0448\u0430",
        };
        const wchar_t* const englishLabels[] = {
            L"Native", L"2 times smaller", L"3 times smaller", L"4 times smaller",
        };
        const auto devices = CombinedDevices();
        // Every connected tablet has its own resolution submenu, named after its monitor.
        bool listedTablet{};
        for (DWORD index = 0; index < kMaxMonitors; ++index)
        {
            const auto& serial = g_hosts[index].serial;
            if (serial.empty()) continue;
            const auto resolutionMenu = CreatePopupMenu();
            if (!resolutionMenu) continue;
            // The check mark shows the resolution actually streamed: the Host falls back to a
            // larger divisor when the tablet decoder cannot play the chosen one, and divisors the
            // decoder cannot play are disabled.
            const auto selected = DeviceResolutionDivisor(serial);
            const auto streaming = IsHostRunning(g_hosts[index]);
            const auto active = DeviceValue(serial, L"ActiveDivisor", 0);
            const auto shown = streaming && active >= 1 && active <= kMaxResolutionDivisor ? active : selected;
            const auto minimum = std::clamp<DWORD>(DeviceValue(serial, L"MinimumDivisor", 1), 1, kMaxResolutionDivisor);
            const auto ignoreDecoderLimit = DeviceIgnoresDecoderLimit(serial);
            for (DWORD divisor = 1; divisor <= kMaxResolutionDivisor; ++divisor)
            {
                std::wstring label = (ukrainian ? ukrainianLabels : englishLabels)[divisor - 1];
                const auto mode = DeviceMode(serial, divisor);
                if (mode)
                    label += L" (" + std::to_wstring(mode->width) + L"\u00D7" + std::to_wstring(mode->height) + L")";
                const auto unsupported = divisor < minimum;
                if (unsupported)
                {
                    label += ukrainian
                        ? L" \u2014 \u043D\u0435 \u043F\u0456\u0434\u0442\u0440\u0438\u043C\u0443\u0454 \u0434\u0435\u043A\u043E\u0434\u0435\u0440"
                        : L" \u2014 not supported by the decoder";
                }
                (void)AppendMenuW(
                    resolutionMenu,
                    MF_STRING | (divisor == shown ? MF_CHECKED : MF_UNCHECKED) |
                        (unsupported && !ignoreDecoderLimit ? MF_GRAYED : 0),
                    kResolutionCommandBase + index * kResolutionCommandStride + divisor,
                    label.c_str());
            }
            const auto commandBase = kResolutionCommandBase + index * kResolutionCommandStride;
            const auto fps = DeviceFps(serial);
            (void)AppendMenuW(resolutionMenu, MF_SEPARATOR, 0, nullptr);
            (void)AppendMenuW(
                resolutionMenu,
                MF_STRING | (fps == 60 ? MF_CHECKED : MF_UNCHECKED),
                commandBase + kFps60CommandOffset,
                ukrainian ? L"60 \u0413\u0446" : L"60 Hz");
            (void)AppendMenuW(
                resolutionMenu,
                MF_STRING | (fps == 30 ? MF_CHECKED : MF_UNCHECKED),
                commandBase + kFps30CommandOffset,
                ukrainian ? L"30 \u0413\u0446" : L"30 Hz");
            (void)AppendMenuW(resolutionMenu, MF_SEPARATOR, 0, nullptr);
            (void)AppendMenuW(
                resolutionMenu,
                MF_STRING | (ignoreDecoderLimit ? MF_CHECKED : MF_UNCHECKED),
                commandBase + kIgnoreDecoderLimitCommandOffset,
                ukrainian
                    ? L"\u0406\u0433\u043d\u043e\u0440\u0443\u0432\u0430\u0442\u0438 \u043e\u0431\u043c\u0435\u0436\u0435\u043d\u043d\u044f \u0434\u0435\u043a\u043e\u0434\u0435\u0440\u0430"
                    : L"Ignore the decoder limit");
            const auto device = std::find_if(devices.begin(), devices.end(), [&](const AdbDevice& candidate)
            {
                return candidate.serial == serial;
            });
            const auto name = device != devices.end() && !device->model.empty()
                ? device->model + L" (" + serial + L")"
                : serial;
            auto title = (ukrainian ? L"\u041c\u043e\u043d\u0456\u0442\u043e\u0440 " : L"Monitor ") +
                std::to_wstring(index + 1) + L": " + name;
            if (const auto mode = DeviceMode(serial, shown); mode && streaming)
            {
                title += L" \u2014 " + std::to_wstring(mode->width) + L"\u00d7" + std::to_wstring(mode->height);
                const auto activeFps = DeviceValue(serial, L"ActiveFps", 0);
                if (activeFps != 0)
                    title += L" @ " + std::to_wstring(activeFps) + (ukrainian ? L" \u0413\u0446" : L" Hz");
            }
            (void)AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(resolutionMenu), title.c_str());
            listedTablet = true;
        }
        if (!listedTablet)
        {
            (void)AppendMenuW(
                menu,
                MF_STRING | MF_GRAYED,
                0,
                ukrainian
                    ? L"\u041f\u043b\u0430\u043d\u0448\u0435\u0442\u0438 \u043d\u0435 \u043f\u0456\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u0456"
                    : L"No tablets connected");
        }
        // Wi-Fi: tablets found on the network that are not paired yet, and the paired ones.
        g_pairCandidates.clear();
        g_forgetCandidates = tabletdisplay::wifi::LoadPairings();
        {
            std::set<std::wstring> paired;
            for (const auto& tablet : g_forgetCandidates) paired.insert(tabletdisplay::wifi::ToHex(tablet.id));
            std::lock_guard lock(g_devicesMutex);
            for (const auto& [id, seen] : g_wifiTablets)
            {
                if (!paired.contains(id) && g_pairCandidates.size() < kMaxWifiMenuItems)
                    g_pairCandidates.push_back(seen.tablet);
            }
        }
        (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (UINT index = 0; index < g_pairCandidates.size(); ++index)
        {
            const auto label = (ukrainian ? L"\u041f\u0456\u0434\u043a\u043b\u044e\u0447\u0438\u0442\u0438 \u043f\u043e Wi-Fi: " : L"Pair over Wi-Fi: ") +
                g_pairCandidates[index].name + L" (" + g_pairCandidates[index].address + L")";
            (void)AppendMenuW(menu, MF_STRING, kPairCommandBase + index, label.c_str());
        }
        if (g_pairCandidates.empty())
        {
            (void)AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                (ukrainian ? L"Wi-Fi: \u043d\u043e\u0432\u0438\u0445 \u043f\u043b\u0430\u043d\u0448\u0435\u0442\u0456\u0432 \u043d\u0435 \u0437\u043d\u0430\u0439\u0434\u0435\u043d\u043e" : L"Wi-Fi: no new tablets found"));
        }
        if (!g_forgetCandidates.empty())
        {
            const auto forgetMenu = CreatePopupMenu();
            if (forgetMenu)
            {
                for (UINT index = 0; index < g_forgetCandidates.size() && index < kMaxWifiMenuItems; ++index)
                {
                    (void)AppendMenuW(forgetMenu, MF_STRING, kForgetCommandBase + index,
                        g_forgetCandidates[index].name.c_str());
                }
                (void)AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(forgetMenu),
                    (ukrainian ? L"\u0417\u0430\u0431\u0443\u0442\u0438 Wi-Fi \u043f\u043b\u0430\u043d\u0448\u0435\u0442" : L"Forget a Wi-Fi tablet"));
            }
        }
        (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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

    std::wstring WidenUtf8(const std::string& text)
    {
        if (text.empty()) return {};
        const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring output(static_cast<std::size_t>(std::max(size, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), size);
        return output;
    }

    // Pairs one tablet on a worker thread: the user compares the six-digit code on both screens
    // (doc/PROTOCOL.md section 10.3).
    void PairTablet(HWND window, tabletdisplay::wifi::DiscoveredTablet tablet)
    {
        // One pairing at a time; the previous worker has finished once the flag is clear.
        if (g_pairingActive.exchange(true)) return;
        if (g_pairingWorker.joinable()) g_pairingWorker.join();
        g_pairingWorker = std::jthread([window, tablet]
        {
            namespace wifi = tabletdisplay::wifi;
            const auto ukrainian = IsUkrainianUi();
            std::wstring result;
            UINT icon = MB_ICONINFORMATION;
            SOCKET socket = INVALID_SOCKET;
            try
            {
                socket = wifi::ConnectTcp(tablet.address, tablet.port, 3'000);
                auto paired = wifi::Pair(socket, wifi::WindowsId(), wifi::WindowsName(),
                    [&](const std::string& code, const std::wstring& tabletName)
                    {
                        const auto spaced = WidenUtf8(code.substr(0, 3) + " " + code.substr(3));
                        const auto text = (ukrainian ? L"\u041f\u0435\u0440\u0435\u0432\u0456\u0440\u0442\u0435, \u0449\u043e \u043d\u0430 \u043f\u043b\u0430\u043d\u0448\u0435\u0442\u0456 \u00ab" : L"Check that the tablet \u201c") +
                            tabletName + (ukrainian ? L"\u00bb \u043f\u043e\u043a\u0430\u0437\u0430\u043d\u043e \u043a\u043e\u0434\n\n" : L"\u201d shows the code\n\n") + spaced +
                            (ukrainian ? L"\n\n\u041a\u043e\u0434\u0438 \u0437\u0431\u0456\u0433\u0430\u044e\u0442\u044c\u0441\u044f? \u041f\u0456\u0434\u0442\u0432\u0435\u0440\u0434\u0456\u0442\u044c \u0442\u0430\u043a\u043e\u0436 \u043d\u0430 \u043f\u043b\u0430\u043d\u0448\u0435\u0442\u0456." : L"\n\nDo the codes match? Confirm on the tablet as well.");
                        return MessageBoxW(nullptr, text.c_str(), L"TabletDisplay Wi-Fi",
                            MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
                    });
                if (paired)
                {
                    paired->address = tablet.address;
                    paired->port = tablet.port;
                    wifi::SavePairing(*paired);
                    result = (ukrainian ? L"\u041f\u043b\u0430\u043d\u0448\u0435\u0442 \u00ab" : L"The tablet \u201c") + paired->name +
                        (ukrainian ? L"\u00bb \u043f\u0456\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u043e \u043f\u043e Wi-Fi." : L"\u201d is paired over Wi-Fi.");
                }
                else
                {
                    result = (ukrainian ? L"\u041f\u0456\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u043d\u044f \u0441\u043a\u0430\u0441\u043e\u0432\u0430\u043d\u043e." : L"Pairing was cancelled.");
                    icon = MB_ICONWARNING;
                }
            }
            catch (const std::exception& exception)
            {
                result = (ukrainian ? L"\u041d\u0435 \u0432\u0434\u0430\u043b\u043e\u0441\u044f \u043f\u0456\u0434\u043a\u043b\u044e\u0447\u0438\u0442\u0438 \u043f\u043b\u0430\u043d\u0448\u0435\u0442: " : L"Pairing failed: ") + WidenUtf8(exception.what());
                icon = MB_ICONERROR;
            }
            if (socket != INVALID_SOCKET) closesocket(socket);
            MessageBoxW(nullptr, result.c_str(), L"TabletDisplay Wi-Fi", MB_OK | icon | MB_TOPMOST | MB_SETFOREGROUND);
            g_pairingActive = false;
            (void)PostMessageW(window, kPairingFinishedMessage, 0, 0);
        });
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
            // A locked session releases the tablets like turned-off displays, so they may sleep.
            (void)WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
            AddTrayIcon(window);
            g_deviceWatcher = std::jthread(WatchDevices, window);
            g_wifiWatcher = std::jthread(WatchWifiTablets, window);
            return 0;
        case kPairingFinishedMessage:
            ReconcileHosts(window);
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK && !g_sessionLocked)
            {
                g_sessionLocked = true;
                StopAllHosts(window);
            }
            else if (wParam == WTS_SESSION_UNLOCK && g_sessionLocked)
            {
                g_sessionLocked = false;
                ReconcileHosts(window);
            }
            return 0;
        case kDevicesMessage:
            ReconcileHosts(window);
            return 0;
        case kHostsStoppedMessage:
            HostsStopped(wParam, lParam);
            ReconcileHosts(window);
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
                            ReconcileHosts(window);
                        else
                            StopAllHosts(window);
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
            if (LOWORD(wParam) > kResolutionCommandBase &&
                LOWORD(wParam) < kResolutionCommandBase + kMaxMonitors * kResolutionCommandStride)
            {
                const DWORD index = (LOWORD(wParam) - kResolutionCommandBase) / kResolutionCommandStride;
                const DWORD offset = (LOWORD(wParam) - kResolutionCommandBase) % kResolutionCommandStride;
                const auto serial = g_hosts[index].serial;
                if (serial.empty() || g_hosts[index].stopping) return 0;
                bool changed{};
                if (offset >= 1 && offset <= kMaxResolutionDivisor)
                {
                    changed = offset != DeviceResolutionDivisor(serial) &&
                        SetDeviceResolutionDivisor(serial, offset);
                }
                else if (offset == kFps60CommandOffset || offset == kFps30CommandOffset)
                {
                    const DWORD fps = offset == kFps60CommandOffset ? 60 : 30;
                    changed = fps != DeviceFps(serial) && SetDeviceValue(serial, L"Fps", fps);
                }
                else if (offset == kIgnoreDecoderLimitCommandOffset)
                {
                    changed = SetDeviceValue(
                        serial, L"IgnoreDecoderLimit", DeviceIgnoresDecoderLimit(serial) ? 0 : 1);
                }
                if (changed)
                {
                    // The Host chooses the monitor mode when it connects, so restart only its Host.
                    std::array<bool, kMaxMonitors> selected{};
                    selected[index] = true;
                    // ReconcileHosts starts the new Host once kHostsStoppedMessage frees the monitor.
                    StopHosts(window, selected);
                }
                return 0;
            }
            if (LOWORD(wParam) >= kPairCommandBase && LOWORD(wParam) < kPairCommandBase + kMaxWifiMenuItems)
            {
                const auto index = LOWORD(wParam) - kPairCommandBase;
                if (index < g_pairCandidates.size()) PairTablet(window, g_pairCandidates[index]);
                return 0;
            }
            if (LOWORD(wParam) >= kForgetCommandBase && LOWORD(wParam) < kForgetCommandBase + kMaxWifiMenuItems)
            {
                const auto index = LOWORD(wParam) - kForgetCommandBase;
                if (index < g_forgetCandidates.size())
                {
                    const auto serial = kWifiSerialPrefix + tabletdisplay::wifi::ToHex(g_forgetCandidates[index].id);
                    tabletdisplay::wifi::RemovePairing(g_forgetCandidates[index].id);
                    std::array<bool, kMaxMonitors> selected{};
                    for (DWORD slot = 0; slot < kMaxMonitors; ++slot) selected[slot] = g_hosts[slot].serial == serial;
                    StopHosts(window, selected);
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
            g_deviceWatcher.request_stop();
            g_wifiWatcher.request_stop();
            (void)WTSUnRegisterSessionNotification(window);
            if (g_displayPowerNotification)
            {
                (void)UnregisterPowerSettingNotification(g_displayPowerNotification);
                g_displayPowerNotification = nullptr;
            }
            (void)Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
            if (g_trayIcon.hIcon) DestroyIcon(g_trayIcon.hIcon);
            StopAllHostsNow();
            if (g_deviceWatcher.joinable()) g_deviceWatcher.join();
            if (g_wifiWatcher.joinable()) g_wifiWatcher.join();
            if (g_pairingWorker.joinable()) g_pairingWorker.detach();
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

    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
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
    for (auto& host : g_hosts)
    {
        if (host.stopEvent) CloseHandle(host.stopEvent);
    }
    CloseHandle(g_instanceMutex);
    return static_cast<int>(message.wParam);
}
