#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

class AntivirusManager {
public:
    static AntivirusManager& Instance();

    bool Start();
    void Stop();

    // This is intentionally a demonstration state/control path, not a file
    // scanner. It proves that the licensed functionality is server-side gated.
    long GetStatus(const std::wstring& userSid, bool& running);
    void Reevaluate(const std::wstring& userSid);

private:
    AntivirusManager() = default;
    ~AntivirusManager() = default;
    AntivirusManager(const AntivirusManager&) = delete;
    AntivirusManager& operator=(const AntivirusManager&) = delete;

    void Worker();

    std::mutex stateMutex_;
    std::map<std::wstring, bool> states_;
    std::mutex waitMutex_;
    std::condition_variable wake_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
