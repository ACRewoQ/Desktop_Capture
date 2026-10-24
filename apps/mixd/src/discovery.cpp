#include "discovery.hpp"

#include "dc/protocol/constants.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windns.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#else
#include <unistd.h>
#endif

namespace dc::mixd {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int len = int(wcslen(w));
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}

// DNS-SD instance labels may hold dots only escaped; keep them plain.
std::string label(std::string s) {
    for (auto& c : s)
        if (c == '.') c = '-';
    if (s.size() > 60) s.resize(60);
    return s.empty() ? "mixd" : s;
}

const wchar_t* kServiceSuffix = L"._dcmix._tcp.local";
const wchar_t* kBrowseName = L"_dcmix._tcp.local";

class WinDiscovery : public Discovery {
public:
    ~WinDiscovery() override { stop(); }

    bool start(const Service& self, std::function<void(std::vector<DiscoveredPeer>)> onChange,
               std::string* error) override {
        self_ = self;
        onChange_ = std::move(onChange);
        if (!registerService(error)) return false;
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        if (running_.exchange(false)) {
            {
                std::lock_guard lock(mutex_);
                cv_.notify_all();
            }
            if (thread_.joinable()) thread_.join();
        }
        cancelBrowse();
        deregister();
    }

    void rename(const std::string& instance) override {
        std::lock_guard lock(regMutex_);
        if (label(instance) == label(self_.instance)) return;
        self_.instance = instance;
        deregisterLocked();
        std::string err;
        registerLocked(&err);
    }

private:
    struct Entry {
        DiscoveredPeer peer;
        std::chrono::steady_clock::time_point seen;
    };
    struct ResolveCtx {
        WinDiscovery* self;
        std::wstring name;
        DNS_SERVICE_CANCEL cancel{};
    };

    bool registerService(std::string* error) {
        std::lock_guard lock(regMutex_);
        return registerLocked(error);
    }

    bool registerLocked(std::string* error) {
        wchar_t host[256]{};
        DWORD hostLen = 256;
        GetComputerNameExW(ComputerNameDnsHostname, host, &hostLen);
        regName_ = widen(label(self_.instance)) + kServiceSuffix;
        regHost_ = std::wstring(host) + L".local";
        const std::wstring proto = std::to_wstring(proto::kProtocolVersion);
        const std::wstring id = widen(self_.id), name = widen(self_.instance);
        PCWSTR keys[] = {L"proto", L"id", L"name"};
        PCWSTR values[] = {proto.c_str(), id.c_str(), name.c_str()};
        instance_ = DnsServiceConstructInstance(regName_.c_str(), regHost_.c_str(), nullptr, nullptr, self_.port, 0, 0,
                                                3, keys, values);
        if (!instance_) {
            if (error) *error = "DnsServiceConstructInstance failed";
            return false;
        }
        reg_ = {};
        reg_.Version = DNS_QUERY_REQUEST_VERSION1;
        reg_.InterfaceIndex = 0;
        reg_.pServiceInstance = instance_;
        reg_.pRegisterCompletionCallback = &WinDiscovery::onRegistered;
        reg_.pQueryContext = this;
        reg_.unicastEnabled = FALSE;
        const DWORD st = DnsServiceRegister(&reg_, &regCancel_);
        if (st != DNS_REQUEST_PENDING) {
            DnsServiceFreeInstance(instance_);
            instance_ = nullptr;
            if (error) *error = "DnsServiceRegister failed (" + std::to_string(st) + "); Windows 10 1809 or later is required";
            return false;
        }
        registered_ = true;
        return true;
    }

    void deregister() {
        std::lock_guard lock(regMutex_);
        deregisterLocked();
    }

    void deregisterLocked() {
        if (registered_) {
            DnsServiceDeRegister(&reg_, nullptr);
            registered_ = false;
        }
        if (instance_) {
            DnsServiceFreeInstance(instance_);
            instance_ = nullptr;
        }
    }

    static VOID WINAPI onRegistered(DWORD, PVOID, PDNS_SERVICE_INSTANCE inst) {
        if (inst) DnsServiceFreeInstance(inst);
    }

    void startBrowse() {
        cancelBrowse();
        browse_ = {};
        browse_.Version = DNS_QUERY_REQUEST_VERSION1;
        browse_.InterfaceIndex = 0;
        browse_.QueryName = kBrowseName;
        browse_.pBrowseCallback = &WinDiscovery::onBrowse;
        browse_.pQueryContext = this;
        browsing_ = DnsServiceBrowse(&browse_, &browseCancel_) == DNS_REQUEST_PENDING;
    }

    void cancelBrowse() {
        if (browsing_) {
            DnsServiceBrowseCancel(&browseCancel_);
            browsing_ = false;
        }
    }

    static VOID WINAPI onBrowse(DWORD status, PVOID ctx, PDNS_RECORD records) {
        auto* self = static_cast<WinDiscovery*>(ctx);
        if (status == ERROR_SUCCESS) {
            // the browse callback always gets wide records, whatever UNICODE says
            for (auto* r = reinterpret_cast<DNS_RECORDW*>(records); r; r = r->pNext)
                if (r->wType == DNS_TYPE_PTR && r->Data.PTR.pNameHost) self->resolve(r->Data.PTR.pNameHost);
        }
        if (records) DnsRecordListFree(records, DnsFreeRecordList);
    }

    void resolve(const wchar_t* name) {
        if (!running_) return;
        if (std::wstring(name) == regName_) return; // ourselves
        auto* ctx = new ResolveCtx{this, name};
        DNS_SERVICE_RESOLVE_REQUEST req{};
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex = 0;
        req.QueryName = ctx->name.data();
        req.pResolveCompletionCallback = &WinDiscovery::onResolved;
        req.pQueryContext = ctx;
        {
            std::lock_guard lock(mutex_);
            ++resolving_;
        }
        if (DnsServiceResolve(&req, &ctx->cancel) != DNS_REQUEST_PENDING) {
            std::lock_guard lock(mutex_);
            --resolving_;
            cv_.notify_all();
            delete ctx;
        }
    }

    static VOID WINAPI onResolved(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE inst) {
        auto* ctx = static_cast<ResolveCtx*>(context);
        WinDiscovery* self = ctx->self;
        if (status == ERROR_SUCCESS && inst && inst->ip4Address) {
            DiscoveredPeer p;
            for (DWORD i = 0; i < inst->dwPropertyCount; ++i) {
                const auto k = narrow(inst->keys[i]);
                if (k == "id") p.id = narrow(inst->values[i]);
                else if (k == "name") p.name = narrow(inst->values[i]);
            }
            const auto a = *inst->ip4Address; // network byte order
            const auto* b = reinterpret_cast<const unsigned char*>(&a);
            p.host = std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
                     std::to_string(b[3]);
            p.port = inst->wPort;
            if (!p.id.empty() && p.id != self->self_.id) {
                std::lock_guard lock(self->mutex_);
                self->entries_[p.id] = {p, std::chrono::steady_clock::now()};
                self->dirty_ = true;
            }
        }
        if (inst) DnsServiceFreeInstance(inst);
        {
            std::lock_guard lock(self->mutex_);
            --self->resolving_;
            self->cv_.notify_all();
        }
        delete ctx;
    }

    void run() {
        using namespace std::chrono;
        auto nextBrowse = steady_clock::now();
        std::unique_lock lock(mutex_);
        while (running_) {
            const auto now = steady_clock::now();
            if (now >= nextBrowse) {
                lock.unlock();
                startBrowse(); // a fresh query every 10 s: answers refresh the entries
                lock.lock();
                nextBrowse = now + seconds(10);
            }
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (now - it->second.seen > seconds(45)) {
                    it = entries_.erase(it);
                    dirty_ = true;
                } else {
                    ++it;
                }
            }
            if (dirty_) {
                dirty_ = false;
                std::vector<DiscoveredPeer> list;
                for (const auto& [id, e] : entries_) list.push_back(e.peer);
                lock.unlock();
                if (onChange_) onChange_(std::move(list));
                lock.lock();
            }
            cv_.wait_for(lock, milliseconds(500));
        }
        // resolve callbacks may still be running
        cv_.wait_for(lock, seconds(3), [&] { return resolving_ == 0; });
    }

    Service self_;
    std::function<void(std::vector<DiscoveredPeer>)> onChange_;
    std::mutex regMutex_;
    std::wstring regName_, regHost_;
    PDNS_SERVICE_INSTANCE instance_ = nullptr;
    DNS_SERVICE_REGISTER_REQUEST reg_{};
    DNS_SERVICE_CANCEL regCancel_{};
    bool registered_ = false;
    DNS_SERVICE_BROWSE_REQUEST browse_{};
    DNS_SERVICE_CANCEL browseCancel_{};
    bool browsing_ = false;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, Entry> entries_;
    bool dirty_ = false;
    int resolving_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace

std::unique_ptr<Discovery> makeDiscovery() { return std::make_unique<WinDiscovery>(); }

std::string computerName() {
    wchar_t buf[256]{};
    DWORD n = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) return "mixd";
    return narrow(buf);
}

#else

std::unique_ptr<Discovery> makeDiscovery() { return nullptr; }

std::string computerName() {
    char buf[256]{};
    if (gethostname(buf, sizeof buf - 1) != 0) return "mixd";
    return buf;
}

#endif

} // namespace dc::mixd
