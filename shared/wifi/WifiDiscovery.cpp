#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>

#include "WifiDiscovery.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>

#pragma comment(lib, "dnsapi.lib")

namespace tabletdisplay::wifi
{
    namespace
    {
        constexpr wchar_t kServiceName[] = L"_tabletdisplay._tcp.local";

        struct BrowseContext
        {
            std::mutex mutex;
            std::condition_variable cancelled;
            bool finished{};
            std::set<std::wstring> instances;
        };

        VOID WINAPI OnBrowse(DWORD status, PVOID context, PDNS_RECORD records)
        {
            auto* browse = static_cast<BrowseContext*>(context);
            {
                std::lock_guard lock(browse->mutex);
                if (status == ERROR_SUCCESS)
                {
                    for (auto* record = records; record != nullptr; record = record->pNext)
                    {
                        if (record->wType == DNS_TYPE_PTR && record->Data.PTR.pNameHost != nullptr)
                            browse->instances.insert(record->Data.PTR.pNameHost);
                    }
                }
                else if (status == ERROR_CANCELLED)
                {
                    browse->finished = true;
                }
            }
            if (records != nullptr) DnsRecordListFree(records, DnsFreeRecordList);
            if (status == ERROR_CANCELLED) browse->cancelled.notify_all();
        }

        struct ResolveContext
        {
            std::mutex mutex;
            std::condition_variable done;
            bool finished{};
            DiscoveredTablet tablet;
        };

        VOID WINAPI OnResolve(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
        {
            auto* resolve = static_cast<ResolveContext*>(context);
            {
                std::lock_guard lock(resolve->mutex);
                if (status == ERROR_SUCCESS && instance != nullptr)
                {
                    resolve->tablet.port = instance->wPort;
                    if (instance->ip4Address != nullptr)
                    {
                        IN_ADDR address{};
                        address.S_un.S_addr = *instance->ip4Address;
                        wchar_t text[INET_ADDRSTRLEN]{};
                        if (InetNtopW(AF_INET, &address, text, INET_ADDRSTRLEN) != nullptr)
                            resolve->tablet.address = text;
                    }
                    for (DWORD index = 0; index < instance->dwPropertyCount; ++index)
                    {
                        if (instance->keys[index] != nullptr && instance->values[index] != nullptr &&
                            wcscmp(instance->keys[index], L"id") == 0)
                            resolve->tablet.id = instance->values[index];
                    }
                }
                resolve->finished = true;
            }
            if (instance != nullptr) DnsServiceFreeInstance(instance);
            resolve->done.notify_all();
        }

        // "TB-X606F._tabletdisplay._tcp.local" -> "TB-X606F".
        std::wstring InstanceLabel(const std::wstring& instance)
        {
            const auto suffix = instance.find(L"._tabletdisplay.");
            return suffix == std::wstring::npos ? instance : instance.substr(0, suffix);
        }
    }

    std::vector<DiscoveredTablet> DiscoverTablets(unsigned browseMs)
    {
        auto browse = std::make_unique<BrowseContext>();
        DNS_SERVICE_BROWSE_REQUEST request{};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.QueryName = kServiceName;
        request.pBrowseCallback = OnBrowse;
        request.pQueryContext = browse.get();
        DNS_SERVICE_CANCEL cancel{};
        if (DnsServiceBrowse(&request, &cancel) != DNS_REQUEST_PENDING) return {};
        Sleep(browseMs);
        (void)DnsServiceBrowseCancel(&cancel);

        std::set<std::wstring> instances;
        {
            // The cancelled browse reports ERROR_CANCELLED last; only then may the context go away.
            std::unique_lock lock(browse->mutex);
            const bool finished = browse->cancelled.wait_for(
                lock, std::chrono::seconds(2), [&] { return browse->finished; });
            instances = browse->instances;
            lock.unlock();
            if (!finished) (void)browse.release(); // Leak rather than let a late callback crash.
        }
        std::vector<DiscoveredTablet> tablets;
        for (const auto& instance : instances)
        {
            auto resolve = std::make_unique<ResolveContext>();
            resolve->tablet.name = InstanceLabel(instance);
            DNS_SERVICE_RESOLVE_REQUEST resolveRequest{};
            resolveRequest.Version = DNS_QUERY_REQUEST_VERSION1;
            resolveRequest.QueryName = const_cast<PWSTR>(instance.c_str());
            resolveRequest.pResolveCompletionCallback = OnResolve;
            resolveRequest.pQueryContext = resolve.get();
            DNS_SERVICE_CANCEL resolveCancel{};
            if (DnsServiceResolve(&resolveRequest, &resolveCancel) != DNS_REQUEST_PENDING) continue;
            std::unique_lock lock(resolve->mutex);
            if (!resolve->done.wait_for(lock, std::chrono::seconds(2), [&] { return resolve->finished; }))
            {
                lock.unlock();
                (void)DnsServiceResolveCancel(&resolveCancel);
                // The callback still runs after a cancel; wait for it before the context goes away.
                lock.lock();
                if (!resolve->done.wait_for(lock, std::chrono::seconds(2), [&] { return resolve->finished; }))
                {
                    lock.unlock();
                    (void)resolve.release(); // A late callback may still use it: leak rather than crash.
                    continue;
                }
            }
            const auto tablet = resolve->tablet;
            lock.unlock();
            if (!tablet.address.empty() && tablet.port != 0 && tablet.id.size() == 32)
                tablets.push_back(tablet);
        }
        return tablets;
    }
}
