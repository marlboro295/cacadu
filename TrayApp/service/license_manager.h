#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct PublicLicenseState {
    bool licensed = false;
    long long expiresUnix = 0;
};

class LicenseManager {
public:
    static LicenseManager& Instance();

    bool Start();
    void Stop();
    void Clear(const std::wstring& userSid);
    bool RefreshNow(const std::wstring& userSid);
    long Activate(const std::wstring& userSid, const std::wstring& code);
    PublicLicenseState PublicState(const std::wstring& userSid) const;
    bool AntivirusAllowed(const std::wstring& userSid) const;

private:
    struct Context {
        mutable std::mutex mutex;
        std::wstring ticket;
        long long expires = 0;
        long long refreshAt = 0;
        std::uint64_t epoch = 0;
        std::uint64_t revision = 0;
    };

    LicenseManager() = default;
    ~LicenseManager() = default;
    LicenseManager(const LicenseManager&) = delete;
    LicenseManager& operator=(const LicenseManager&) = delete;

    std::shared_ptr<Context> GetOrCreate(const std::wstring& userSid);
    std::shared_ptr<Context> Find(const std::wstring& userSid) const;
    std::vector<std::pair<std::wstring, std::shared_ptr<Context>>> Snapshot()
        const;
    void ClearContext(const std::shared_ptr<Context>& context);
    void ClearContextIfCurrent(const std::shared_ptr<Context>& context,
                               std::uint64_t expectedEpoch,
                               std::uint64_t expectedRevision);
    bool BeginOperation(const std::shared_ptr<Context>& context,
                        std::uint64_t& epoch,
                        std::uint64_t& revision);
    bool StoreIfCurrent(const std::shared_ptr<Context>& context,
                        std::uint64_t expectedEpoch,
                        std::uint64_t expectedRevision,
                        const std::wstring& ticket,
                        long long expires,
                        long long refreshAt);
    void ClearAll();
    void Worker();

    mutable std::mutex registryMutex_;
    mutable std::map<std::wstring, std::shared_ptr<Context>> contexts_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
