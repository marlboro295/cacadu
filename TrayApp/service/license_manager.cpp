#include "license_manager.h"

#include "api_config.h"
#include "auth_manager.h"
#include "http_client.h"
#include "json_utils.h"

enum {
    APP_OK = 0,
    APP_UNAUTHORIZED = 1,
    APP_HTTP_ERROR = 2,
    APP_BAD_RESPONSE = 3,
    APP_NO_LICENSE = 4,
    APP_ACTIVATION_FAILED = 5,
    APP_RPC_ERROR = 100
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

bool ParseLicenseResponse(const std::string& body,
                          std::wstring& ticket,
                          long long& expires,
                          long long& refreshAt) {
    if (!JsonUtil::IsValidJson(body)) return false;

    std::string ticketUtf8;
    if (!JsonUtil::ExtractString(body, "ticket", ticketUtf8) ||
        ticketUtf8.empty()) {
        return false;
    }

    if (!JsonUtil::ExtractInt64(body, "expires_at", expires) ||
        expires <= JsonUtil::UnixNow()) {
        return false;
    }

    bool hasRefreshAt = JsonUtil::ExtractInt64(body, "refresh_at", refreshAt);
    if (!hasRefreshAt || refreshAt <= 0 || refreshAt >= expires)
        refreshAt = expires > 60 ? expires - 60 : JsonUtil::UnixNow();

    ticket = JsonUtil::Utf8ToWide(ticketUtf8);
    return !ticket.empty();
}

}

LicenseManager& LicenseManager::Instance() {
    static LicenseManager instance;
    return instance;
}

std::shared_ptr<LicenseManager::Context>
LicenseManager::GetOrCreate(const std::wstring& userSid) {
    if (userSid.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto& context = contexts_[userSid];
    if (!context) context = std::make_shared<Context>();
    return context;
}

std::shared_ptr<LicenseManager::Context>
LicenseManager::Find(const std::wstring& userSid) const {
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto it = contexts_.find(userSid);
    return it == contexts_.end() ? nullptr : it->second;
}

std::vector<std::pair<std::wstring, std::shared_ptr<LicenseManager::Context>>>
LicenseManager::Snapshot() const {
    std::vector<std::pair<std::wstring, std::shared_ptr<Context>>> result;
    std::lock_guard<std::mutex> lock(registryMutex_);
    result.reserve(contexts_.size());
    for (const auto& item : contexts_)
        result.push_back(item);
    return result;
}

bool LicenseManager::Start() {
    if (running_.exchange(true)) return true;
    worker_ = std::thread(&LicenseManager::Worker, this);
    return true;
}

void LicenseManager::Stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    ClearAll();
}

void LicenseManager::ClearContext(const std::shared_ptr<Context>& context) {
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->mutex);
    ++context->epoch;
    ++context->revision;
    ClearSecret(context->ticket);
    context->expires = 0;
    context->refreshAt = 0;
}

void LicenseManager::ClearContextIfCurrent(
    const std::shared_ptr<Context>& context,
    std::uint64_t expectedEpoch,
    std::uint64_t expectedRevision) {
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->epoch != expectedEpoch ||
        context->revision != expectedRevision) return;
    ++context->epoch;
    ++context->revision;
    ClearSecret(context->ticket);
    context->expires = 0;
    context->refreshAt = 0;
}

bool LicenseManager::StoreIfCurrent(const std::shared_ptr<Context>& context,
                                    std::uint64_t expectedEpoch,
                                    std::uint64_t expectedRevision,
                                    const std::wstring& ticket,
                                    long long expires,
                                    long long refreshAt) {
    if (!context || ticket.empty() || expires <= JsonUtil::UnixNow())
        return false;

    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->epoch != expectedEpoch ||
        context->revision != expectedRevision) return false;
    ClearSecret(context->ticket);
    context->ticket = ticket;
    context->expires = expires;
    context->refreshAt = refreshAt;
    return true;
}

bool LicenseManager::BeginOperation(const std::shared_ptr<Context>& context,
                                    std::uint64_t& epoch,
                                    std::uint64_t& revision) {
    if (!context) return false;
    std::lock_guard<std::mutex> lock(context->mutex);
    epoch = context->epoch;
    revision = ++context->revision;
    return true;
}

void LicenseManager::Clear(const std::wstring& userSid) {
    ClearContext(Find(userSid));
}

void LicenseManager::ClearAll() {
    for (const auto& item : Snapshot())
        ClearContext(item.second);
}

PublicLicenseState LicenseManager::PublicState(
    const std::wstring& userSid) const {
    auto context = Find(userSid);
    if (!context) return {};

    std::lock_guard<std::mutex> lock(context->mutex);
    const long long now = JsonUtil::UnixNow();
    return {
        !context->ticket.empty() && context->expires > now,
        context->expires
    };
}

bool LicenseManager::AntivirusAllowed(const std::wstring& userSid) const {
    if (!AuthManager::Instance().PublicState(userSid).authenticated)
        return false;
    return PublicState(userSid).licensed;
}

bool LicenseManager::RefreshNow(const std::wstring& userSid) {
    auto context = GetOrCreate(userSid);
    if (!context) return false;

    std::wstring accessToken;
    if (!AuthManager::Instance().GetAccessToken(userSid, accessToken)) {
        return false;
    }

    std::uint64_t requestEpoch = 0;
    std::uint64_t requestRevision = 0;
    if (!BeginOperation(context, requestEpoch, requestRevision)) return false;

    HttpResponse response = HttpClient::GetJson(
        ApiConfig::LicensePath(), accessToken);
    ClearSecret(accessToken);
    if (response.status == 404 || response.status == 204 ||
        response.status == 401 || response.status == 403) {
        ClearContextIfCurrent(context, requestEpoch, requestRevision);
        return false;
    }
    // A temporary transport/5xx failure preserves a still-unexpired ticket,
    // but PublicState remains fail-closed once expires_at has passed.
    if (!response.Ok()) return false;

    std::wstring ticket;
    long long expires = 0;
    long long refreshAt = 0;
    if (!ParseLicenseResponse(response.body, ticket, expires, refreshAt)) {
        ClearContextIfCurrent(context, requestEpoch, requestRevision);
        return false;
    }

    return StoreIfCurrent(context, requestEpoch, requestRevision,
                          ticket, expires, refreshAt);
}

long LicenseManager::Activate(const std::wstring& userSid,
                              const std::wstring& code) {
    if (code.empty()) return APP_BAD_RESPONSE;

    auto context = GetOrCreate(userSid);
    if (!context) return APP_BAD_RESPONSE;

    std::wstring accessToken;
    if (!AuthManager::Instance().GetAccessToken(userSid, accessToken))
        return APP_UNAUTHORIZED;

    std::uint64_t requestEpoch = 0;
    std::uint64_t requestRevision = 0;
    if (!BeginOperation(context, requestEpoch, requestRevision))
        return APP_RPC_ERROR;

    std::string activationCode =
        JsonUtil::Escape(JsonUtil::WideToUtf8(code));
    HttpResponse response = HttpClient::PostJson(
        ApiConfig::ActivatePath(),
        "{\"activation_code\":\"" + activationCode + "\"}",
        accessToken);
    ClearSecret(accessToken);
    ClearSecret(activationCode);
    if (response.status == 400 || response.status == 404 ||
        response.status == 409 || response.status == 422) {
        return APP_ACTIVATION_FAILED;
    }
    if (response.status == 401 || response.status == 403) {
        ClearContextIfCurrent(context, requestEpoch, requestRevision);
        return APP_UNAUTHORIZED;
    }
    if (!response.Ok()) return APP_HTTP_ERROR;

    if (!JsonUtil::IsValidJson(response.body)) {
        ClearContextIfCurrent(context, requestEpoch, requestRevision);
        return APP_BAD_RESPONSE;
    }

    std::string ticketUtf8;
    if (!JsonUtil::ExtractString(response.body, "ticket", ticketUtf8))
        return RefreshNow(userSid) ? APP_OK : APP_NO_LICENSE;

    std::wstring ticket;
    long long expires = 0;
    long long refreshAt = 0;
    if (!ParseLicenseResponse(response.body, ticket, expires, refreshAt)) {
        ClearContextIfCurrent(context, requestEpoch, requestRevision);
        return APP_BAD_RESPONSE;
    }

    return StoreIfCurrent(context, requestEpoch, requestRevision,
                          ticket, expires, refreshAt)
        ? APP_OK
        : APP_NO_LICENSE;
}

void LicenseManager::Worker() {
    while (running_.load()) {
        for (const auto& item : Snapshot()) {
            if (!running_.load()) break;

            {
                std::lock_guard<std::mutex> transitionLock(
                    AuthLicenseTransitionMutex());
                if (!AuthManager::Instance().PublicState(item.first).authenticated) {
                    ClearContext(item.second);
                    continue;
                }
            }

            bool needRefresh = false;
            {
                std::lock_guard<std::mutex> lock(item.second->mutex);
                const long long now = JsonUtil::UnixNow();
                needRefresh = item.second->ticket.empty() ||
                              item.second->expires <= now ||
                              item.second->refreshAt == 0 ||
                              item.second->refreshAt <= now;
            }
            if (needRefresh)
                RefreshNow(item.first);
        }

        for (int i = 0; i < 30 && running_.load(); ++i)
            Sleep(1000);
    }
}
