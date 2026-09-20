#include "antivirus_manager.h"

#include "auth_manager.h"
#include "license_manager.h"

#include <chrono>
#include <vector>

namespace {
enum {
    APP_OK = 0,
    APP_NO_LICENSE = 4,
    APP_RPC_ERROR = 100
};
}

AntivirusManager& AntivirusManager::Instance() {
    static AntivirusManager instance;
    return instance;
}

bool AntivirusManager::Start() {
    if (running_.exchange(true)) return true;
    worker_ = std::thread(&AntivirusManager::Worker, this);
    return true;
}

void AntivirusManager::Stop() {
    running_ = false;
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(stateMutex_);
    states_.clear();
}

long AntivirusManager::GetStatus(const std::wstring& userSid, bool& running) {
    running = false;
    if (userSid.empty()) return APP_RPC_ERROR;

    bool allowed = false;
    {
        std::lock_guard<std::mutex> transitionLock(
            AuthLicenseTransitionMutex());
        allowed = LicenseManager::Instance().AntivirusAllowed(userSid);
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        states_[userSid] = allowed;
    }
    wake_.notify_one();
    running = allowed;
    return allowed ? APP_OK : APP_NO_LICENSE;
}

void AntivirusManager::Reevaluate(const std::wstring& userSid) {
    if (userSid.empty()) return;
    bool allowed = false;
    {
        std::lock_guard<std::mutex> transitionLock(
            AuthLicenseTransitionMutex());
        allowed = LicenseManager::Instance().AntivirusAllowed(userSid);
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = states_.find(userSid);
        if (it != states_.end()) it->second = allowed;
    }
    wake_.notify_one();
}

void AntivirusManager::Worker() {
    while (running_.load()) {
        std::vector<std::wstring> users;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            users.reserve(states_.size());
            for (const auto& item : states_) users.push_back(item.first);
        }

        for (const auto& userSid : users) {
            if (!running_.load()) break;
            const bool allowed = LicenseManager::Instance().AntivirusAllowed(userSid);
            std::lock_guard<std::mutex> lock(stateMutex_);
            auto it = states_.find(userSid);
            if (it != states_.end()) it->second = allowed;
        }

        std::unique_lock<std::mutex> lock(waitMutex_);
        wake_.wait_for(lock, std::chrono::seconds(1),
                       [this] { return !running_.load(); });
    }
}
