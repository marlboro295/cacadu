#include "auth_manager.h"

#include "api_config.h"
#include "http_client.h"
#include "json_utils.h"
#include "license_manager.h"

#include <chrono>

enum {
    APP_OK = 0,
    APP_UNAUTHORIZED = 1,
    APP_HTTP_ERROR = 2,
    APP_BAD_RESPONSE = 3
};

namespace {

void ClearSecret(std::wstring& value) {
    if (!value.empty())
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    value.clear();
}

void ClearSecret(std::string& value) {
    if (!value.empty())
        SecureZeroMemory(value.data(), value.size());
    value.clear();
}

}

namespace {
std::mutex g_authLicenseTransitionMutex;
}

std::mutex& AuthLicenseTransitionMutex() {
    return g_authLicenseTransitionMutex;
}

AuthManager& AuthManager::Instance() {
    static AuthManager instance;
    return instance;
}

std::shared_ptr<AuthManager::Context>
AuthManager::GetOrCreate(const std::wstring& userSid) {
    if (userSid.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto& context = contexts_[userSid];
    if (!context) context = std::make_shared<Context>();
    return context;
}

std::shared_ptr<AuthManager::Context>
AuthManager::Find(const std::wstring& userSid) const {
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto it = contexts_.find(userSid);
    return it == contexts_.end() ? nullptr : it->second;
}

std::vector<std::pair<std::wstring, std::shared_ptr<AuthManager::Context>>>
AuthManager::Snapshot() const {
    std::vector<std::pair<std::wstring, std::shared_ptr<Context>>> result;
    std::lock_guard<std::mutex> lock(registryMutex_);
    result.reserve(contexts_.size());
    for (const auto& item : contexts_)
        result.push_back(item);
    return result;
}

bool AuthManager::Start() {
    if (running_.exchange(true)) return true;
    worker_ = std::thread(&AuthManager::Worker, this);
    return true;
}

void AuthManager::Stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    ClearAll();
}

void AuthManager::ClearContext(const std::shared_ptr<Context>& context) {
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->mutex);
    ++context->epoch;
    ++context->revision;
    context->username.clear();
    ClearSecret(context->access);
    ClearSecret(context->refresh);
    context->accessExp = 0;
    context->refreshExp = 0;
}

void AuthManager::ClearAll() {
    for (const auto& item : Snapshot())
        ClearContext(item.second);
}

long AuthManager::Login(const std::wstring& userSid,
                        const std::wstring& username,
                        const std::wstring& password) {
    auto context = GetOrCreate(userSid);
    if (!context || username.empty() || password.empty())
        return APP_BAD_RESPONSE;

    std::uint64_t requestEpoch = 0;
    std::unique_lock<std::mutex> transitionLock(AuthLicenseTransitionMutex());
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        ++context->epoch;
        ++context->revision;
        context->username.clear();
        ClearSecret(context->access);
        ClearSecret(context->refresh);
        context->accessExp = 0;
        context->refreshExp = 0;
        requestEpoch = context->epoch;
    }
    LicenseManager::Instance().Clear(userSid);
    transitionLock.unlock();

    std::string u = JsonUtil::Escape(JsonUtil::WideToUtf8(username));
    std::string p = JsonUtil::Escape(JsonUtil::WideToUtf8(password));
    HttpResponse response = HttpClient::PostJson(
        ApiConfig::LoginPath(),
        "{\"username\":\"" + u + "\",\"password\":\"" + p + "\"}");
    ClearSecret(p);
    ClearSecret(u);
    if (!response.TransportOk()) return APP_HTTP_ERROR;
    if (response.status == 401 || response.status == 403)
        return APP_UNAUTHORIZED;
    if (!response.Ok()) return APP_HTTP_ERROR;
    if (!JsonUtil::IsValidJson(response.body)) return APP_BAD_RESPONSE;

    std::string access;
    std::string refresh;
    if (!JsonUtil::ExtractString(response.body, "access_token", access) ||
        !JsonUtil::ExtractString(response.body, "refresh_token", refresh) ||
        access.empty() || refresh.empty()) {
        return APP_BAD_RESPONSE;
    }

    const long long accessExp = JsonUtil::JwtExp(access);
    const long long refreshExp = JsonUtil::JwtExp(refresh);
    const long long now = JsonUtil::UnixNow();
    if (accessExp <= now || refreshExp <= now)
        return APP_BAD_RESPONSE;

    std::string serverUser;
    JsonUtil::ExtractString(response.body, "username", serverUser);

    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->epoch != requestEpoch)
            return APP_UNAUTHORIZED;
        context->username = serverUser.empty()
            ? username
            : JsonUtil::Utf8ToWide(serverUser);
        context->access = JsonUtil::Utf8ToWide(access);
        context->refresh = JsonUtil::Utf8ToWide(refresh);
        context->accessExp = accessExp;
        context->refreshExp = refreshExp;
    }

    LicenseManager::Instance().RefreshNow(userSid);
    return APP_OK;
}

long AuthManager::Logout(const std::wstring& userSid) {
    std::unique_lock<std::mutex> transitionLock(AuthLicenseTransitionMutex());
    auto context = Find(userSid);
    if (context) {
        std::lock_guard<std::mutex> lock(context->mutex);
        ++context->epoch;
        ++context->revision;
        context->username.clear();
        ClearSecret(context->access);
        ClearSecret(context->refresh);
        context->accessExp = 0;
        context->refreshExp = 0;
    }
    LicenseManager::Instance().Clear(userSid);
    return APP_OK;
}

bool AuthManager::InvalidateIfCurrent(
    const std::wstring& userSid,
    const std::shared_ptr<Context>& context,
    std::uint64_t expectedEpoch,
    std::uint64_t expectedRevision) {
    if (!context) return false;

    std::unique_lock<std::mutex> transitionLock(AuthLicenseTransitionMutex());
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->epoch != expectedEpoch ||
            context->revision != expectedRevision) return false;
        ++context->epoch;
        ++context->revision;
        context->username.clear();
        ClearSecret(context->access);
        ClearSecret(context->refresh);
        context->accessExp = 0;
        context->refreshExp = 0;
    }
    // The transition lock prevents a concurrent Login from installing a new
    // auth/license pair between this epoch check and the license clear.
    LicenseManager::Instance().Clear(userSid);
    return true;
}

PublicAuthState AuthManager::PublicState(const std::wstring& userSid) const {
    auto context = Find(userSid);
    if (!context) return {};

    std::lock_guard<std::mutex> lock(context->mutex);
    const long long now = JsonUtil::UnixNow();
    return {
        !context->access.empty() && context->accessExp > now,
        context->username
    };
}

bool AuthManager::GetAccessToken(const std::wstring& userSid,
                                 std::wstring& token) const {
    token.clear();
    auto context = Find(userSid);
    if (!context) return false;

    std::lock_guard<std::mutex> lock(context->mutex);
    const long long now = JsonUtil::UnixNow();
    if (context->access.empty() || context->accessExp <= now) {
        return false;
    }
    token = context->access;
    return true;
}

bool AuthManager::Refresh(const std::wstring& userSid,
                          const std::shared_ptr<Context>& context) {
    if (!context) return false;

    std::wstring refresh;
    std::uint64_t requestEpoch = 0;
    std::uint64_t requestRevision = 0;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->refresh.empty()) return false;
        refresh = context->refresh;
        requestEpoch = context->epoch;
        requestRevision = ++context->revision;
    }

    std::string refreshJson =
        JsonUtil::Escape(JsonUtil::WideToUtf8(refresh));
    HttpResponse response = HttpClient::PostJson(
        ApiConfig::RefreshPath(),
        "{\"refresh_token\":\"" + refreshJson + "\"}");
    ClearSecret(refreshJson);
    ClearSecret(refresh);
    if (!response.TransportOk()) return false;
    if (response.status == 401 || response.status == 403) {
        InvalidateIfCurrent(userSid, context, requestEpoch, requestRevision);
        return false;
    }
    if (!response.Ok()) return false;
    if (!JsonUtil::IsValidJson(response.body)) {
        InvalidateIfCurrent(userSid, context, requestEpoch, requestRevision);
        return false;
    }

    std::string access;
    if (!JsonUtil::ExtractString(response.body, "access_token", access) ||
        access.empty()) {
        InvalidateIfCurrent(userSid, context, requestEpoch, requestRevision);
        return false;
    }

    const long long accessExp = JsonUtil::JwtExp(access);
    if (accessExp <= JsonUtil::UnixNow()) {
        InvalidateIfCurrent(userSid, context, requestEpoch, requestRevision);
        return false;
    }

    std::string newRefresh;
    const bool hasNewRefresh =
        JsonUtil::ExtractString(response.body, "refresh_token", newRefresh);
    long long newRefreshExp = 0;
    if (hasNewRefresh) {
        newRefreshExp = JsonUtil::JwtExp(newRefresh);
        if (newRefresh.empty() || newRefreshExp <= JsonUtil::UnixNow()) {
            InvalidateIfCurrent(userSid, context, requestEpoch,
                                requestRevision);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->epoch != requestEpoch ||
            context->revision != requestRevision)
            return false;
        context->access = JsonUtil::Utf8ToWide(access);
        context->accessExp = accessExp;
        if (hasNewRefresh) {
            ClearSecret(context->refresh);
            context->refresh = JsonUtil::Utf8ToWide(newRefresh);
            context->refreshExp = newRefreshExp;
        }
    }
    return true;
}

void AuthManager::Worker() {
    while (running_.load()) {
        for (const auto& item : Snapshot()) {
            if (!running_.load()) break;

            bool expiredRefresh = false;
            bool shouldRefresh = false;
            {
                std::lock_guard<std::mutex> lock(item.second->mutex);
                const long long now = JsonUtil::UnixNow();
                expiredRefresh = !item.second->refresh.empty() &&
                                 item.second->refreshExp > 0 &&
                                 item.second->refreshExp <= now;
                shouldRefresh = !item.second->access.empty() &&
                                item.second->accessExp > 0 &&
                                item.second->accessExp <= now + 60;
            }

            if (expiredRefresh)
                Logout(item.first);
            else if (shouldRefresh)
                Refresh(item.first, item.second);
        }

        for (int i = 0; i < 10 && running_.load(); ++i)
            Sleep(1000);
    }
}
