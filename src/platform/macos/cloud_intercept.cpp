#include "cloud_intercept.h"
#include "json.h"
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

static std::string g_steamPath;
static std::atomic<uint32_t> g_accountId{0};
static std::mutex g_mutex;
static std::unordered_set<uint32_t> g_namespaceApps;

static bool DigitsOnly(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

static void LoadSlssteamAdditionalApps(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;

    bool inAdditional = false;
    bool disableCloudSeen = false;
    bool disableCloud = false;
    std::vector<uint32_t> parsedApps;
    std::string line;

    auto trim = [](std::string value) {
        const size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        const size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    };

    auto parseBool = [](const std::string& value, bool* out) {
        if (value == "yes" || value == "true" || value == "1" ||
            value == "Yes" || value == "True" || value == "TRUE") {
            *out = true;
            return true;
        }
        if (value == "no" || value == "false" || value == "0" ||
            value == "No" || value == "False" || value == "FALSE") {
            *out = false;
            return true;
        }
        return false;
    };

    auto appendApp = [&](const std::string& text) {
        std::string value = trim(text);
        const size_t comment = value.find('#');
        if (comment != std::string::npos)
            value = trim(value.substr(0, comment));
        if (!DigitsOnly(value)) return;
        try {
            const uint32_t appId = static_cast<uint32_t>(std::stoul(value));
            if (appId != 0)
                parsedApps.push_back(appId);
        } catch (...) {
        }
    };

    while (std::getline(f, line)) {
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;

        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Handle list items before top-level key detection. SLSsteam's YAML
        // commonly uses unindented "- 123" entries under AdditionalApps.
        if (trimmed.rfind("- ", 0) == 0) {
            if (inAdditional)
                appendApp(trimmed.substr(2));
            continue;
        }

        const size_t colon = trimmed.find(':');
        if (colon == std::string::npos) {
            inAdditional = false;
            continue;
        }

        const std::string key = trim(trimmed.substr(0, colon));
        std::string value = trim(trimmed.substr(colon + 1));
        const size_t comment = value.find('#');
        if (comment != std::string::npos)
            value = trim(value.substr(0, comment));

        if (key == "DisableCloud") {
            bool parsed = false;
            if (parseBool(value, &parsed)) {
                disableCloudSeen = true;
                disableCloud = parsed;
            }
            inAdditional = false;
            continue;
        }

        if (key == "AdditionalApps") {
            inAdditional = true;

            // Also accept AdditionalApps: [123, 456].
            if (!value.empty() && value.front() == '[' && value.back() == ']') {
                const std::string body = value.substr(1, value.size() - 2);
                std::string cur;
                for (size_t i = 0; i <= body.size(); ++i) {
                    const char c = (i < body.size()) ? body[i] : ',';
                    if (c >= '0' && c <= '9') {
                        cur += c;
                    } else if (!cur.empty()) {
                        appendApp(cur);
                        cur.clear();
                    }
                }
                inAdditional = false;
            }
            continue;
        }

        if (first == 0)
            inAdditional = false;
    }

    size_t added = 0;
    if (disableCloudSeen && !disableCloud) {
        for (uint32_t appId : parsedApps) {
            if (g_namespaceApps.insert(appId).second)
                ++added;
        }
    }

    LOG("[Mac] SLSsteam config: %s; DisableCloud=%s; AdditionalApps parsed=%zu added=%zu",
        path.c_str(),
        disableCloudSeen ? (disableCloud ? "yes" : "no") : "missing",
        parsedApps.size(), added);
}

static void LoadApps(const std::string& path){
    std::ifstream f(path); if(!f)return;
    std::string s((std::istreambuf_iterator<char>(f)),{});
    size_t p=s.find("\"namespace_apps\""); if(p==std::string::npos)p=s.find("\"AdditionalApps\"");
    if(p==std::string::npos)return;
    size_t a=s.find('[',p),b=s.find(']',a); if(a==std::string::npos||b==std::string::npos)return;
    std::string body=s.substr(a+1,b-a-1), cur;
    for(size_t i=0;i<=body.size();i++){
        char c=(i<body.size()?body[i]:',');
        if(c>='0'&&c<='9') cur+=c;
        else if(!cur.empty()){try{g_namespaceApps.insert((uint32_t)std::stoul(cur));}catch(...){ }cur.clear();}
    }
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

        if (depth == 1 && trimmed.size() > 2 && trimmed[0] == '"' ) {
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
void InitMac(){
    std::lock_guard<std::mutex> lk(g_mutex);
    g_namespaceApps.clear();
    g_steamPath=XdgHome()+"/Library/Application Support/Steam";
    const std::string configRoot = XdgConfigHome();
    const std::string home = XdgHome();

    LoadApps(configRoot+"/CloudRedirect/config.json");

    // SLSsteam's documented config is ~/.config/SLSsteam/config.yaml on Linux.
    // macOS builds may instead follow the platform's Application Support layout,
    // so probe all known layouts rather than silently assuming one of them.
    const std::vector<std::string> slssteamConfigPaths = {
        home + "/.config/SLSsteam/config.yaml",
        configRoot + "/SLSsteam/config.yaml",
        configRoot + "/Steam/SLSsteam/config.yaml",
        g_steamPath + "/SLSsteam/config.yaml",
        g_steamPath + "/config/SLSsteam/config.yaml",
    };

    std::unordered_set<std::string> seenConfigPaths;
    for (const auto& path : slssteamConfigPaths) {
        if (!seenConfigPaths.insert(path).second)
            continue;
        std::error_code pathEc;
        const bool exists = std::filesystem::is_regular_file(path, pathEc);
        LOG("[Mac] SLSsteam config probe: %s -> %s%s",
            path.c_str(),
            exists ? "FOUND" : "MISSING",
            pathEc ? " (filesystem error)" : "");
        if (exists)
            LoadSlssteamAdditionalApps(path);
    }

    const std::filesystem::path userdata = std::filesystem::path(g_steamPath) / "userdata";
    const uint32_t selectedAccount = ResolveActiveAccountId(g_steamPath);
    if (selectedAccount != 0)
        g_accountId.store(selectedAccount, std::memory_order_relaxed);

    std::error_code ec;
    std::vector<uint32_t> userdataAccounts;
    for (const auto& entry : std::filesystem::directory_iterator(userdata, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        if (!DigitsOnly(name)) continue;
        try {
            uint32_t account = static_cast<uint32_t>(std::stoull(name));
            if (account != 0)
                userdataAccounts.push_back(account);
        } catch (...) {
            continue;
        }
    }

    uint32_t activeAccount = g_accountId.load(std::memory_order_relaxed);
    if (activeAccount == 0 && userdataAccounts.size() == 1) {
        activeAccount = userdataAccounts.front();
        g_accountId.store(activeAccount, std::memory_order_relaxed);
    } else if (activeAccount == 0 && userdataAccounts.size() > 1) {
        LOG("[Mac] Multiple Steam userdata accounts found but loginusers.vdf did not identify one; refusing ambiguous account selection");
    }

    std::string ids;
    for (uint32_t appId : g_namespaceApps) {
        if (!ids.empty()) ids += ",";
        ids += std::to_string(appId);
    }
    if (ids.empty()) ids = "<none configured>";
    LOG("[Mac] Target policy: configured namespace AppIDs only (%s)", ids.c_str());
    LOG("[Mac] Steam path: %s; account=%u; namespaceApps=%zu; target=namespace-only",
        g_steamPath.c_str(), g_accountId.load(), g_namespaceApps.size());
}
bool IsNamespaceApp(uint32_t id){
    if (id == 0) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_namespaceApps.count(id) > 0;
}
bool HasNamespaceApps(){
    std::lock_guard<std::mutex> lk(g_mutex);
    return !g_namespaceApps.empty();
}
std::vector<uint32_t> GetNamespaceApps(){std::lock_guard<std::mutex>lk(g_mutex);return std::vector<uint32_t>(g_namespaceApps.begin(),g_namespaceApps.end());}
void RegisterNamespaceApp(uint32_t id){std::lock_guard<std::mutex>lk(g_mutex);if(id)g_namespaceApps.insert(id);}
std::string GetSteamPath(){std::lock_guard<std::mutex>lk(g_mutex);return g_steamPath;}
uint32_t GetAccountId(){return g_accountId.load(std::memory_order_relaxed);}
void SetAccountId(uint32_t id){g_accountId.store(id,std::memory_order_relaxed);}
void SetSteamPath(const std::string& p){std::lock_guard<std::mutex>lk(g_mutex);g_steamPath=p;}
void Shutdown(){}
}
