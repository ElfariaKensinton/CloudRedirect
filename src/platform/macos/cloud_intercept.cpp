#include "cloud_intercept.h"
#include "log.h"
#include "xdg.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cctype>
#include <cstdlib>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstring>

static std::string g_steamPath;
static std::atomic<uint32_t> g_accountId{0};
static std::mutex g_mutex;
static std::unordered_set<uint32_t> g_namespaceApps;

static std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    if (first != 0) value.erase(0, first);
    return value;
}

static bool DigitsOnly(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

static bool ParseBoolScalar(std::string value, bool& out) {
    value = Trim(value);
    while (value.size() >= 2 &&
           ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
        value = Trim(value);
    }

    std::string lower;
    lower.reserve(value.size());
    for (char c : value)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower == "yes" || lower == "true" || lower == "on" || lower == "1") {
        out = true;
        return true;
    }
    if (lower == "no" || lower == "false" || lower == "off" || lower == "0") {
        out = false;
        return true;
    }
    return false;
}

static void ParseAppIds(std::string value, std::vector<uint32_t>& out) {
    const size_t comment = value.find('#');
    if (comment != std::string::npos)
        value.resize(comment);
    value = Trim(value);

    std::string current;
    auto flush = [&] {
        if (current.empty()) return;
        try {
            const unsigned long id = std::stoul(current);
            if (id > 0 && id <= 0xFFFFFFFFUL)
                out.push_back(static_cast<uint32_t>(id));
        } catch (...) {}
        current.clear();
    };

    for (char c : value) {
        if (c >= '0' && c <= '9')
            current.push_back(c);
        else
            flush();
    }
    flush();
}

// Returns: -1 = config missing/unreadable, 0 = DisableCloud enabled/missing,
// 1 = usable config. Matches the Linux SLSsteam policy: CloudRedirect only
// manages apps explicitly listed under AdditionalApps when DisableCloud=false.
static int LoadSlssteamConfig(const std::string& path, int* outAdded) {
    std::ifstream f(path);
    if (!f) return -1;

    bool haveDisableCloud = false;
    bool disableCloud = true;
    bool inAdditional = false;
    std::vector<uint32_t> parsedApps;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        const size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.resize(comment);

        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
            continue;

        const std::string trimmed = line.substr(first);

        if (trimmed.rfind("DisableCloud:", 0) == 0) {
            bool value = false;
            if (ParseBoolScalar(trimmed.substr(std::strlen("DisableCloud:")), value)) {
                disableCloud = value;
                haveDisableCloud = true;
            }
            inAdditional = false;
            continue;
        }

        if (trimmed.rfind("AdditionalApps:", 0) == 0) {
            inAdditional = true;
            ParseAppIds(trimmed.substr(std::strlen("AdditionalApps:")), parsedApps);
            continue;
        }

        if (!inAdditional)
            continue;

        // YAML list item form:
        // AdditionalApps:
        //   - 123
        if (trimmed.rfind("- ", 0) == 0 || trimmed == "-")
        {
            ParseAppIds(trimmed.size() > 1 ? trimmed.substr(1) : std::string(), parsedApps);
            continue;
        }

        // A new top-level YAML key ends the AdditionalApps section.
        if (first == 0 && trimmed.find(':') != std::string::npos)
            inAdditional = false;
    }

    if (!haveDisableCloud || disableCloud)
        return 0;

    int added = 0;
    for (uint32_t appId : parsedApps) {
        if (g_namespaceApps.insert(appId).second)
            ++added;
    }

    if (outAdded) *outAdded = added;
    return 1;
}

static uint32_t ResolveActiveAccountId(const std::string& steamPath) {
    std::ifstream f(std::filesystem::path(steamPath) / "config" / "loginusers.vdf");
    if (!f) return 0;

    uint32_t mostRecent = 0;
    uint32_t autoLogin = 0;
    uint32_t latest = 0;
    uint64_t latestTs = 0;
    uint64_t currentSid = 0;
    int depth = 0;
    bool inUser = false;

    std::string line;
    while (std::getline(f, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const std::string trimmed = line.substr(first);

        if (trimmed == "{") { ++depth; continue; }
        if (trimmed == "}") {
            --depth;
            if (depth == 1) inUser = false;
            continue;
        }

        if (depth == 1 && trimmed.size() > 2 && trimmed[0] == '"') {
            size_t end = trimmed.find('"', 1);
            if (end != std::string::npos) {
                const std::string key = trimmed.substr(1, end - 1);
                char* endp = nullptr;
                const uint64_t sid = std::strtoull(key.c_str(), &endp, 10);
                if (endp == key.c_str() + key.size() && sid > 76561197960265728ULL) {
                    currentSid = sid;
                    inUser = true;
                }
            }
        }

        if (inUser && depth == 2) {
            if (trimmed.find("\"MostRecent\"") != std::string::npos &&
                trimmed.find("\"1\"") != std::string::npos) {
                mostRecent = static_cast<uint32_t>(currentSid & 0xFFFFFFFFu);
            }
            if (trimmed.find("\"AutoLogin\"") != std::string::npos &&
                trimmed.find("\"1\"") != std::string::npos) {
                autoLogin = static_cast<uint32_t>(currentSid & 0xFFFFFFFFu);
            }
            if (trimmed.find("\"Timestamp\"") != std::string::npos) {
                size_t q1 = trimmed.find('"', trimmed.find("\"Timestamp\"") + 11);
                if (q1 != std::string::npos) {
                    size_t q2 = trimmed.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        uint64_t ts = 0;
                        try { ts = std::stoull(trimmed.substr(q1 + 1, q2 - q1 - 1)); } catch (...) {}
                        if (ts > latestTs) {
                            latestTs = ts;
                            latest = static_cast<uint32_t>(currentSid & 0xFFFFFFFFu);
                        }
                    }
                }
            }
        }
    }

    if (mostRecent) return mostRecent;
    if (autoLogin) return autoLogin;
    return latest;
}

namespace CloudIntercept {

void InitMac() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_namespaceApps.clear();
    g_steamPath = XdgHome() + "/Library/Application Support/Steam";

    const std::vector<std::string> configPaths = {
        XdgConfigHome() + "/SLSsteam/config.yaml",
        XdgHome() + "/.config/SLSsteam/config.yaml",
        g_steamPath + "/SLSsteam/config.yaml",
    };

    bool foundConfig = false;
    bool cloudAllowed = false;
    for (const auto& configPath : configPaths) {
        int added = 0;
        const int result = LoadSlssteamConfig(configPath, &added);
        if (result < 0)
            continue;

        foundConfig = true;
        if (result == 0) {
            LOG("[Mac] SLSsteam config found but DisableCloud is enabled/missing: %s",
                configPath.c_str());
        } else {
            cloudAllowed = true;
            LOG("[Mac] SLSsteam config: %s (AdditionalApps +%d)",
                configPath.c_str(), added);
        }
        break;
    }

    if (!foundConfig)
        LOG("[Mac] No SLSsteam config found; no namespace apps will be managed");
    else if (!cloudAllowed)
        LOG("[Mac] Namespace cloud policy disabled by SLSsteam config");

    const uint32_t selectedAccount = ResolveActiveAccountId(g_steamPath);
    if (selectedAccount != 0)
        g_accountId.store(selectedAccount, std::memory_order_relaxed);

    if (g_accountId.load(std::memory_order_relaxed) == 0) {
        const std::filesystem::path userdata =
            std::filesystem::path(g_steamPath) / "userdata";
        std::error_code ec;
        std::vector<uint32_t> userdataAccounts;
        for (const auto& entry : std::filesystem::directory_iterator(userdata, ec)) {
            if (ec) break;
            if (!entry.is_directory()) continue;
            const std::string name = entry.path().filename().string();
            if (!DigitsOnly(name)) continue;
            try {
                const uint32_t account = static_cast<uint32_t>(std::stoull(name));
                if (account != 0)
                    userdataAccounts.push_back(account);
            } catch (...) {
                continue;
            }
        }

        if (userdataAccounts.size() == 1)
            g_accountId.store(userdataAccounts.front(), std::memory_order_relaxed);
        else if (userdataAccounts.size() > 1)
            LOG("[Mac] Multiple Steam userdata accounts found but loginusers.vdf did not identify one; refusing ambiguous account selection");
    }

    std::string ids;
    for (uint32_t appId : g_namespaceApps) {
        if (!ids.empty()) ids += ",";
        ids += std::to_string(appId);
    }
    if (ids.empty()) ids = "<none>";
    LOG("[Mac] Target policy: SLSsteam AdditionalApps only (%s)", ids.c_str());
    LOG("[Mac] Steam path: %s; account=%u; namespaceApps=%zu; target=namespace-only",
        g_steamPath.c_str(), g_accountId.load(), g_namespaceApps.size());
}

bool IsNamespaceApp(uint32_t id) {
    if (id == 0) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_namespaceApps.count(id) > 0;
}

bool HasNamespaceApps() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return !g_namespaceApps.empty();
}

std::vector<uint32_t> GetNamespaceApps() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return std::vector<uint32_t>(g_namespaceApps.begin(), g_namespaceApps.end());
}

void RegisterNamespaceApp(uint32_t id) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_namespaceApps.insert(id).second)
        LOG("[Mac] Dynamically registered namespace app: %u", id);
}

std::string GetSteamPath() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_steamPath;
}

uint32_t GetAccountId() {
    return g_accountId.load(std::memory_order_relaxed);
}

void SetAccountId(uint32_t id) {
    g_accountId.store(id, std::memory_order_relaxed);
    LOG("[Mac] Account ID set: %u", id);
}

void SetSteamPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_steamPath = path;
}

void Shutdown() {
    LOG("[Mac] CloudIntercept shutdown");
}

} // namespace CloudIntercept
