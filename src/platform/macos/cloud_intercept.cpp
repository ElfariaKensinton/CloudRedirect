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
    std::string line;
    bool inAdditional = false;
    while (std::getline(f, line)) {
        size_t first = line.find_first_not_of(" \t");
        const std::string trimmed = first == std::string::npos ? std::string() : line.substr(first);
        if (first == 0 && !trimmed.empty() &&
            trimmed != "AdditionalApps:" &&
            trimmed.rfind("AdditionalApps:", 0) != 0) {
            inAdditional = false;
        }
        if (trimmed == "AdditionalApps:" || trimmed.rfind("AdditionalApps:", 0) == 0) {
            inAdditional = true;
            continue;
        }
        if (!inAdditional) continue;
        if (trimmed.rfind("- ", 0) != 0) continue;
        std::string value = trimmed.substr(2);
        const size_t comment = value.find('#');
        if (comment != std::string::npos) value.resize(comment);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        if (!DigitsOnly(value)) continue;
        try { g_namespaceApps.insert(static_cast<uint32_t>(std::stoul(value))); } catch (...) {}
    }
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
    LoadApps(XdgConfigHome()+"/CloudRedirect/config.json");
    // SLSsteam keeps its native config under ~/.config on macOS as well.
    // Also retain the historical fallback paths used by older wrappers/installers.
    LoadSlssteamAdditionalApps(XdgHome()+"/.config/SLSsteam/config.yaml");
    LoadSlssteamAdditionalApps(XdgConfigHome()+"/SLSsteam/config.yaml");
    LoadSlssteamAdditionalApps(g_steamPath + "/SLSsteam/config.yaml");

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
