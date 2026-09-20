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

struct PublicAuthState {
    bool authenticated = false;
    std::wstring username;
};

// Serializes auth/license state transitions without holding either manager's
// internal mutex while an unrelated manager calls back into the other one.
std::mutex& AuthLicenseTransitionMutex();

class AuthManager {
public:
    static AuthManager& Instance();

    bool Start();
    void Stop();
    long Login(const std::wstring& userSid,
               const std::wstring& username,
               const std::wstring& password);
    long Logout(const std::wstring& userSid);
    PublicAuthState PublicState(const std::wstring& userSid) const;
    bool GetAccessToken(const std::wstring& userSid,
                        std::wstring& token) const;

private:
    struct Context {
        mutable std::mutex mutex;
        std::wstring username;
        std::wstring access;
        std::wstring refresh;
        long long accessExp = 0;
        long long refreshExp = 0;
        std::uint64_t epoch = 0;
        std::uint64_t revision = 0;
    };

    AuthManager() = default;
    ~AuthManager() = default;
    AuthManager(const AuthManager&) = delete;
    AuthManager& operator=(const AuthManager&) = delete;

    std::shared_ptr<Context> GetOrCreate(const std::wstring& userSid);
    std::shared_ptr<Context> Find(const std::wstring& userSid) const;
    std::vector<std::pair<std::wstring, std::shared_ptr<Context>>> Snapshot()
        const;
    bool Refresh(const std::wstring& userSid,
                 const std::shared_ptr<Context>& context);
    bool InvalidateIfCurrent(const std::wstring& userSid,
                             const std::shared_ptr<Context>& context,
                             std::uint64_t expectedEpoch,
                             std::uint64_t expectedRevision);
    void ClearContext(const std::shared_ptr<Context>& context);
    void ClearAll();
    void Worker();

    mutable std::mutex registryMutex_;
    mutable std::map<std::wstring, std::shared_ptr<Context>> contexts_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
