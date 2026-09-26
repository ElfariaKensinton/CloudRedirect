#include "cloud_hooks.h"
#include "cloud_intercept.h"
#include "cloud_storage.h"
#include "local_storage.h"
#include "local_metadata_store.h"
#include "pending_ops_journal.h"
#include "http_server.h"
#include "cloud_provider.h"
#include "cloud_provider_base.h"
#include "rpc_handlers.h"
#include "metadata_sync.h"
#include "stats_hooks.h"
#include "stats_handlers.h"
#include "stats_store.h"
#include "protobuf.h"
#include "json.h"
#include "log.h"
#include "xdg.h"
#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
#include <cstring>
#include <optional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include <cstdlib>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

using RawServerMethodFn=bool(*)(void*,const char*,const void*,unsigned int,void*,int*);
using RawServerNotificationFn=bool(*)(void*,const char*,void*,int*);
using TypedServerMethodFn=bool(*)(void*,const char*,void*,void*,void*);
using TypedServerNotificationFn=bool(*)(void*,const char*,void*,void*);
using CloudEnabledFn=bool(*)(void*,unsigned int);
using ParseFromArrayFn=bool(*)(void*,const void*,int);
using SerializeToArrayFn=bool(*)(const void*,void*,int);
using ByteSizeFn=int(*)(const void*);

static std::atomic<ParseFromArrayFn> g_parseFromArray{nullptr};
static std::atomic<SerializeToArrayFn> g_serializeToArray{nullptr};

static std::atomic<RawServerMethodFn> g_origServerMethod{nullptr};
static std::atomic<RawServerNotificationFn> g_origServerNotification{nullptr};
static std::atomic<TypedServerMethodFn> g_origServerMethodTyped{nullptr};
static std::atomic<TypedServerNotificationFn> g_origServerNotificationTyped{nullptr};
static std::atomic<CloudEnabledFn> g_origCloud{nullptr};
static std::atomic<bool> g_initialized{false},g_shuttingDown{false};
static std::atomic<int> g_hookRefCount{0};
static std::once_flag g_initOnce;
static std::atomic<bool> g_statsSyncEnabled{false};
static std::unique_ptr<std::thread> g_seedThread;
static std::unique_ptr<std::thread> g_cloudPollerThread;
static std::mutex g_seedExitMtx, g_pollerExitMtx;
static std::condition_variable g_seedExitCv, g_pollerExitCv;
static std::atomic<bool> g_seedExited{false};
static std::atomic<bool> g_pollerExited{false};

struct HookGuard {
 bool active = false;
 HookGuard() {
   g_hookRefCount.fetch_add(1, std::memory_order_acq_rel);
   if (g_shuttingDown.load(std::memory_order_acquire)) {
     g_hookRefCount.fetch_sub(1, std::memory_order_release);
     return;
   }
   active = true;
 }
 ~HookGuard() {
   if (active) g_hookRefCount.fetch_sub(1, std::memory_order_release);
 }
};

static bool RequiresLocalHttp(const char* method)
{
 return method &&
   (strcmp(method,CloudIntercept::RPC_BEGIN_UPLOAD)==0 ||
    strcmp(method,CloudIntercept::RPC_FILE_DOWNLOAD)==0);
}

static void EnsureInitialized();
static uint32_t ExtractRequestAppId(const char* method,const std::vector<PB::Field>& fields);
static std::optional<CloudIntercept::RpcResult> Dispatch(
    const char* method, uint32_t app, const std::vector<PB::Field>& fields);
static void* FindLoadedImageSymbol(const char* wanted)
{
    if (!wanted || !*wanted)
        return nullptr;

    if (void* p = dlsym(RTLD_DEFAULT, wanted))
        return p;

    // Mach-O string tables store an extra leading '_' for external C/C++ symbols
    // compared with the name accepted by dlsym(). Match both spellings below.
    const uint32_t count = _dyld_image_count();
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto* mh = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
            const char* imageName = _dyld_get_image_name(i);
            if (!mh || mh->magic != MH_MAGIC_64 || !imageName)
                continue;

            const bool preferredImage =
                strstr(imageName, "client.dylib") ||
                strstr(imageName, "steamclient") ||
                strstr(imageName, "SteamClient");
            if ((pass == 0 && !preferredImage) || (pass == 1 && preferredImage))
                continue;

            intptr_t slide = _dyld_get_image_vmaddr_slide(i);
            const load_command* lc =
                reinterpret_cast<const load_command*>(
                    reinterpret_cast<const uint8_t*>(mh) + sizeof(mach_header_64));
            const symtab_command* symtab = nullptr;
            uintptr_t linkeditBase = 0;
            for (uint32_t j = 0; j < mh->ncmds; ++j) {
                if (lc->cmd == LC_SYMTAB)
                    symtab = reinterpret_cast<const symtab_command*>(lc);
                else if (lc->cmd == LC_SEGMENT_64) {
                    const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
                    if (strcmp(seg->segname, SEG_LINKEDIT) == 0)
                        linkeditBase = static_cast<uintptr_t>(seg->vmaddr) +
                                       slide - static_cast<uintptr_t>(seg->fileoff);
                }
                lc = reinterpret_cast<const load_command*>(
                    reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
            }
            if (!symtab || !linkeditBase)
                continue;

            const auto* strtab = reinterpret_cast<const char*>(linkeditBase + symtab->stroff);
            const auto* symbols = reinterpret_cast<const nlist_64*>(linkeditBase + symtab->symoff);
            for (uint32_t j = 0; j < symtab->nsyms; ++j) {
                const auto& n = symbols[j];
                if ((n.n_type & N_TYPE) == N_UNDF || n.n_un.n_strx >= symtab->strsize)
                    continue;

                const char* symbolName = strtab + n.n_un.n_strx;
                const bool nameMatches =
                    strcmp(symbolName, wanted) == 0 ||
                    (wanted[0] == '_' && symbolName[0] == '_' &&
                     strcmp(symbolName + 1, wanted) == 0);
                if (nameMatches && n.n_value != 0) {
                    LOG("[Mac] protobuf helper symbol %s found in %s",
                        wanted, imageName);
                    return reinterpret_cast<void*>(
                        static_cast<uintptr_t>(n.n_value) + slide);
                }
            }
        }
    }
    return nullptr;
}

static void ResolveProtobufHelpers()
{
    if (g_parseFromArray.load(std::memory_order_acquire) &&
        g_serializeToArray.load(std::memory_order_acquire))
        return;

    // Resolve the helpers without assuming a protobuf vtable layout.
    // The typed hook only becomes active when Steam's loaded image exposes
    // the direct SerializeToArray helper; otherwise requests pass through.
    void* parse = FindLoadedImageSymbol(
        "_ZN6google8protobuf11MessageLite14ParseFromArrayEPKvi");
    if (!parse)
        parse = FindLoadedImageSymbol(
            "_ZN6google8protobuf11MessageLite22ParsePartialFromArrayEPKvi");

    void* serialize = FindLoadedImageSymbol(
        "_ZNK6google8protobuf11MessageLite15SerializeToArrayEPvi");

    g_parseFromArray.store(reinterpret_cast<ParseFromArrayFn>(parse), std::memory_order_release);
    g_serializeToArray.store(reinterpret_cast<SerializeToArrayFn>(serialize), std::memory_order_release);
    LOG("[Mac] protobuf typed helpers parse=%p serialize=%p", parse, serialize);
}

static bool SerializeTypedMessage(const void* message, std::vector<uint8_t>& out)
{
    if (!message) return false;
    ResolveProtobufHelpers();

    auto serialize = g_serializeToArray.load(std::memory_order_acquire);
    if (!serialize)
        return false;

    auto* vt = *reinterpret_cast<void***>(const_cast<void*>(message));
    if (!vt)
        return false;

    auto byteSize = reinterpret_cast<ByteSizeFn>(vt[9]);
    if (!byteSize)
        return false;

    const int size = byteSize(message);
    if (size < 0 || size > 64 * 1024 * 1024)
        return false;

    out.resize(static_cast<size_t>(size));
    if (size == 0)
        return true;

    return serialize(message, out.data(), size);
}

namespace CloudHooks {
bool TypedHooksAvailable()
{
    ResolveProtobufHelpers();
    const auto parse = g_parseFromArray.load(std::memory_order_acquire);
    const auto serialize = g_serializeToArray.load(std::memory_order_acquire);
    if (!parse || !serialize)
        return false;

    Dl_info parseInfo{};
    Dl_info serializeInfo{};
    if (!dladdr(reinterpret_cast<void*>(parse), &parseInfo) ||
        !dladdr(reinterpret_cast<void*>(serialize), &serializeInfo) ||
        !parseInfo.dli_fbase || !serializeInfo.dli_fbase ||
        parseInfo.dli_fbase != serializeInfo.dli_fbase) {
        LOG("[Mac] typed protobuf helpers come from different images; disabling typed transport");
        return false;
    }

    const char* image = parseInfo.dli_fname ? parseInfo.dli_fname : "";
    const bool steamImage = strstr(image, "client.dylib") ||
                            strstr(image, "steamclient") ||
                            strstr(image, "SteamClient") ||
                            strstr(image, "steam_osx");
    if (!steamImage) {
        LOG("[Mac] typed protobuf helpers resolved from non-Steam image %s; disabling typed transport",
            image);
        return false;
    }
    return true;
}

}

static bool ParseTypedMessage(void* message, const std::vector<uint8_t>& body)
{
    if (!message) return false;
    ResolveProtobufHelpers();
    auto parse = g_parseFromArray.load(std::memory_order_acquire);
    if (!parse) return false;
    static const uint8_t kEmpty = 0;
    const void* data = body.empty() ? static_cast<const void*>(&kEmpty) : body.data();
    return parse(message, data, static_cast<int>(body.size()));
}

static bool HandleTypedServerMethod(
    void* t, const char* m, void* request, void* response, void* options)
{
    auto orig = g_origServerMethodTyped.load(std::memory_order_acquire);
    if (!orig)
        return false;

    if (m && (strncmp(m, "Cloud.", 6) == 0 ||
              strcmp(m, StatsHandlers::RPC_GET_USER_STATS) == 0 ||
              strcmp(m, StatsHandlers::RPC_GET_LAST_PLAYED) == 0)) {
        LOG("[Mac] typed RPC observed: %s req=%p resp=%p options=%p",
            m, request, response, options);
    }

    // Observation only: do not serialize or mutate the protobuf objects yet.
    // The current Steam macOS protobuf ABI must be identified from live calls
    // before we make this path authoritative.
    return orig(t, m, request, response, options);
}




static std::optional<CloudIntercept::RpcResult> Dispatch(const char*m,uint32_t app,const std::vector<PB::Field>&f){
 using namespace CloudIntercept;
 if(strcmp(m,RPC_GET_CHANGELIST)==0)return HandleGetChangelist(app,f);
 if(strcmp(m,RPC_LAUNCH_INTENT)==0)return HandleLaunchIntent(app,f);
 if(strcmp(m,RPC_SUSPEND_SESSION)==0)return HandleSuspendSession(app,f);
 if(strcmp(m,RPC_RESUME_SESSION)==0)return HandleResumeSession(app,f);
 if(strcmp(m,RPC_QUOTA_USAGE)==0)return HandleQuotaUsage(app,f);
 if(strcmp(m,RPC_BEGIN_BATCH)==0)return HandleBeginBatch(app,f);
 if(strcmp(m,RPC_BEGIN_UPLOAD)==0)return HandleBeginFileUpload(app,f);
 if(strcmp(m,RPC_COMMIT_UPLOAD)==0)return HandleCommitFileUpload(app,f);
 if(strcmp(m,RPC_COMPLETE_BATCH)==0)return HandleCompleteBatch(app,f);
 if(strcmp(m,RPC_FILE_DOWNLOAD)==0)return HandleFileDownload(app,f);
 if(strcmp(m,RPC_DELETE_FILE)==0)return HandleDeleteFile(app,f);
 return std::nullopt;
}
static void EnsureInitialized(){
 if(g_initialized.load(std::memory_order_acquire)||g_shuttingDown.load(std::memory_order_acquire))return;
 std::call_once(g_initOnce,[](){
   if(g_shuttingDown.load(std::memory_order_acquire)) return;

   CloudIntercept::InitMac();
   const std::string root=XdgConfigHome()+"/CloudRedirect/";
   const std::string storage=root+"storage";
   std::unique_ptr<ICloudProvider> provider;
   std::string cfgText;
   {
     std::ifstream f(root+"config.json");
     cfgText.assign(std::istreambuf_iterator<char>(f),{});
   }

   bool statsEnabled = MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
                       MetadataSync::syncPlaytime.load(std::memory_order_relaxed);

   if(!cfgText.empty()){
     auto cfg=Json::Parse(cfgText);
     std::string name=cfg["provider"].str();

     if(cfg["stats_sync_enabled"].type==Json::Type::Bool &&
        !cfg["stats_sync_enabled"].boolean()){
       MetadataSync::syncAchievements.store(false,std::memory_order_relaxed);
       MetadataSync::syncPlaytime.store(false,std::memory_order_relaxed);
     }else{
       if(cfg["sync_achievements"].type==Json::Type::Bool)
         MetadataSync::syncAchievements.store(cfg["sync_achievements"].boolean(),std::memory_order_relaxed);
       if(cfg["sync_playtime"].type==Json::Type::Bool)
         MetadataSync::syncPlaytime.store(cfg["sync_playtime"].boolean(),std::memory_order_relaxed);
     }

     statsEnabled = MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
                    MetadataSync::syncPlaytime.load(std::memory_order_relaxed);

     if(!name.empty()&&name!="local"){
       provider=CreateCloudProvider(name);
       if(provider){
         std::string tokenPath=ResolveProviderTokenPath(root,cfgText,name);
         if(!provider->Init(tokenPath)||!provider->IsAuthenticated())provider.reset();
       }
     }
   }

   g_statsSyncEnabled.store(statsEnabled,std::memory_order_release);
   CloudStorage::Init(root,std::move(provider));
   LocalStorage::Init(storage);
   LocalMetadataStore::Init(storage);
   PendingOpsJournal::Init(storage);
   HttpServer::SetAccountId(CloudIntercept::GetAccountId());
   if(!HttpServer::Start(storage,CloudIntercept::GetAccountId()))
     LOG("[Mac] local cloud HTTP server failed to start; Cloud RPCs will pass through");

   if(statsEnabled){
     StatsStore::SetCloudProvider(
       [](std::unordered_map<uint32_t,std::string>& out)->bool{
         uint32_t accountId=CloudIntercept::GetAccountId();
         if(accountId==0)return false;
         std::vector<uint8_t> data;
         auto fetch=CloudStorage::FetchCloudMetadataStatus(
             accountId,CloudIntercept::kAccountScopeAppId,"stats.json",data);
         if(fetch==CloudStorage::MetadataFetch::Error)return false;
         if(fetch==CloudStorage::MetadataFetch::Missing||data.empty())return true;
         auto root=Json::Parse(std::string(reinterpret_cast<const char*>(data.data()),data.size()));
         if(root.type!=Json::Type::Object)return true;
         for(const auto& [appIdStr,appVal]:root.objVal){
           uint32_t appId=0;
           try{appId=(uint32_t)std::stoul(appIdStr);}catch(...){continue;}
           if(appId)out[appId]=Json::Stringify(appVal);
         }
         return true;
       },
       [](const std::unordered_map<uint32_t,std::string>& all){
         CloudStorage::InflightSyncScope guard;
         if(!guard.entered)return;
         uint32_t accountId=CloudIntercept::GetAccountId();
         if(accountId==0)return;

         Json::Value root=Json::Object();
         std::vector<uint8_t> cur;
         if(CloudStorage::DownloadCloudMetadataWithLegacyFallback(
              accountId,CloudIntercept::kAccountScopeAppId,"stats.json",nullptr,cur)&&!cur.empty()){
           auto parsed=Json::Parse(std::string(reinterpret_cast<const char*>(cur.data()),cur.size()));
           if(parsed.type==Json::Type::Object)root=std::move(parsed);
         }

         auto resetApps=StatsStore::ConsumeResetApps();
         bool changed=false;
         for(const auto& [appId,json]:all){
           if(appId==0)continue;
           std::string key=std::to_string(appId);
           std::string merged;
           if(resetApps.count(appId)) merged=json;
           else {
             std::string baseEntry=root.has(key)?Json::Stringify(root.objVal[key]):std::string();
             merged=StatsStore::MergeAppStatsJson(baseEntry,json);
           }
           auto appVal=Json::Parse(merged);
           if(appVal.type!=Json::Type::Object)continue;
           if(!root.has(key)||!Json::DeepEqual(root.objVal[key],appVal)){
             root.objVal[key]=std::move(appVal);
             changed=true;
           }
         }
         if(changed)
           CloudStorage::UploadCloudMetadataTextAsync(
             accountId,CloudIntercept::kAccountScopeAppId,"stats.json",Json::Stringify(root));
       },
       [](uint32_t appId)->std::string{
         CloudStorage::InflightSyncScope guard;
         if(!guard.entered)return {};
         uint32_t accountId=CloudIntercept::GetAccountId();
         if(accountId==0)return {};
         std::vector<uint8_t> data;
         if(CloudStorage::DownloadCloudMetadataWithLegacyFallback(
              accountId,appId,"stats.json",nullptr,data)&&!data.empty())
           return std::string(reinterpret_cast<const char*>(data.data()),data.size());
         return {};
       },
       [](uint32_t appId)->std::string{
         CloudStorage::InflightSyncScope guard;
         if(!guard.entered)return {};
         uint32_t accountId=CloudIntercept::GetAccountId();
         if(accountId==0)return {};
         std::vector<uint8_t> data;
         if(CloudStorage::DownloadLegacyPlaytimeBlob(accountId,appId,data))
           return std::string(reinterpret_cast<const char*>(data.data()),data.size());
         return {};
       });

     StatsHandlers::SetNamespacePredicate(
       [](uint32_t appId){return CloudIntercept::IsNamespaceApp(appId);});
     StatsStore::SetNamespacePredicate(
       [](uint32_t appId){return CloudIntercept::IsNamespaceApp(appId);});
     StatsStore::SetAccountIdProvider(
       []()->uint32_t{return CloudIntercept::GetAccountId();});
     StatsStore::Init(root,CloudIntercept::GetSteamPath());
     StatsHandlers::Init();

     if(MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
        MetadataSync::syncPlaytime.load(std::memory_order_relaxed)){
       g_seedExited.store(false,std::memory_order_release);
       g_seedThread=std::make_unique<std::thread>([]{
         if(!g_shuttingDown.load(std::memory_order_acquire))
           StatsStore::SeedApps(CloudIntercept::GetNamespaceApps());
         g_seedExited.store(true,std::memory_order_release);
         g_seedExitCv.notify_all();
       });

       g_pollerExited.store(false,std::memory_order_release);
       g_cloudPollerThread=std::make_unique<std::thread>([]{
         while(!g_shuttingDown.load(std::memory_order_acquire)){
           for(int i=0;i<60 && !g_shuttingDown.load(std::memory_order_acquire);++i)
             std::this_thread::sleep_for(std::chrono::seconds(1));
           if(g_shuttingDown.load(std::memory_order_acquire))break;
           const auto apps=StatsStore::GetKnownApps();
           if(MetadataSync::syncPlaytime.load(std::memory_order_relaxed)) {
             StatsStore::RefreshLocalPlaytime();
             auto changed=StatsStore::RefreshFromCloud(apps);
             if(!changed.empty())
               LOG("[Stats] macOS cloud poll advanced %zu app(s)",changed.size());
           }
           if(MetadataSync::syncAchievements.load(std::memory_order_relaxed)) {
             for(uint32_t appId:apps) {
               if(appId) StatsStore::CaptureNativeUnlocks(appId);
             }
           }
         }
         g_pollerExited.store(true,std::memory_order_release);
         g_pollerExitCv.notify_all();
       });
     }
   }

   g_initialized.store(true,std::memory_order_release);
   LOG("[Mac] CloudRedirect initialized account=%u namespaceApps=%zu stats=%d achievements=%d playtime=%d",
       CloudIntercept::GetAccountId(),CloudIntercept::GetNamespaceApps().size(),
       statsEnabled?1:0,
       MetadataSync::syncAchievements.load()?1:0,
       MetadataSync::syncPlaytime.load()?1:0);
 });
}
namespace CloudHooks {
void Initialize(){ EnsureInitialized(); }
void SetOriginalRaw(void* serverMethod,void* serverNotification){
 g_origServerMethod.store((RawServerMethodFn)serverMethod,std::memory_order_release);
 g_origServerNotification.store((RawServerNotificationFn)serverNotification,std::memory_order_release);
}
void SetOriginalTyped(void* serverMethod,void* serverNotification){
 g_origServerMethodTyped.store((TypedServerMethodFn)serverMethod,std::memory_order_release);
 g_origServerNotificationTyped.store((TypedServerNotificationFn)serverNotification,std::memory_order_release);
}
void SetOriginalIsCloudEnabled(void*o){g_origCloud.store((CloudEnabledFn)o,std::memory_order_release);}
void InstallGamesPlayedObserver(uintptr_t,size_t){
 LOG("[Mac] GamesPlayed observer unavailable on this build; playtime uses native stats reconciliation/poller");
}
void BeginShutdown(){
 g_shuttingDown.store(true,std::memory_order_release);
 HttpServer::Stop();

 LOG("[Mac] BeginShutdown: stopping stats workers");
 if(g_seedThread && g_seedThread->joinable()){
   std::unique_lock<std::mutex> lk(g_seedExitMtx);
   g_seedExitCv.wait(lk,[] { return g_seedExited.load(std::memory_order_acquire); });
   lk.unlock();
   g_seedThread->join();
   g_seedThread.reset();
 }
 if(g_cloudPollerThread && g_cloudPollerThread->joinable()){
   std::unique_lock<std::mutex> lk(g_pollerExitMtx);
   g_pollerExitCv.wait(lk,[] { return g_pollerExited.load(std::memory_order_acquire); });
   lk.unlock();
   g_cloudPollerThread->join();
   g_cloudPollerThread.reset();
 }
 if(g_statsSyncEnabled.load(std::memory_order_acquire)) {
   if(MetadataSync::syncPlaytime.load(std::memory_order_relaxed))
     StatsStore::RefreshLocalPlaytime();
   StatsHandlers::Shutdown();
 }

 while(g_hookRefCount.load(std::memory_order_acquire)>0)
   std::this_thread::sleep_for(std::chrono::milliseconds(1));

 CloudStorage::Shutdown();
 LOG("[Mac] BeginShutdown: complete");
}
}

static uint32_t ExtractRequestAppId(const char* method,const std::vector<PB::Field>& fields){
 if(!method) return 0;
 if(strcmp(method,StatsHandlers::RPC_GET_USER_STATS)==0){
   if(auto* f=PB::FindField(fields,2)) return (uint32_t)f->varintVal;
   return 0;
 }
 return CloudRpcUtils::ExtractAppId(method,fields);
}

static void MergeRawLastPlayedResponse(std::string& response,const PB::Writer& body)
{
    const auto& data = body.Data();
    if (data.empty()) return;

    // StatsHandlers emits only the repeated games field (field 1). Protobuf
    // messages are mergeable by concatenating serialized repeated fields, so
    // preserve Steam's server response and append only our local games.
    response.append(reinterpret_cast<const char*>(data.data()), data.size());
}

extern "C" bool hook_ServerMethodTyped(
    void*t,const char*m,void*request,void*response,void*options)
{
    return HandleTypedServerMethod(t, m, request, response, options);
}

extern "C" bool hook_ServerNotificationTyped(
    void*t,const char*m,void*message,void*options)
{
    auto orig=g_origServerNotificationTyped.load(std::memory_order_acquire);
    if(!orig || !m)
        return orig ? orig(t,m,message,options) : false;

    if(strncmp(m,"Cloud.",6)==0)
        LOG("[Mac] typed notification observed: %s msg=%p options=%p",
            m, message, options);

    return orig(t,m,message,options);
}

extern "C" bool hook_SyncSend2(
    void*t,const char*m,const void*buf,unsigned int len,void*response,int*flags)
{
 HookGuard guard;
 auto orig=g_origServerMethod.load(std::memory_order_acquire);
 for(int i=0; !orig && i<1000; ++i){
   std::this_thread::sleep_for(std::chrono::microseconds(100));
   orig=g_origServerMethod.load(std::memory_order_acquire);
 }
 if(!orig||g_shuttingDown.load(std::memory_order_acquire)||!m||!buf)
   return orig?orig(t,m,buf,len,response,flags):false;

 const bool isCloud = strncmp(m,"Cloud.",6)==0;
 const bool isGetUserStats = strcmp(m,StatsHandlers::RPC_GET_USER_STATS)==0;
 const bool isGetLastPlayed = strcmp(m,StatsHandlers::RPC_GET_LAST_PLAYED)==0;
 if(!isCloud && !isGetUserStats && !isGetLastPlayed)
   return orig(t,m,buf,len,response,flags);

 LOG("[Mac] RPC observed: %s body=%u flags=%p", m, len, (void*)flags);
 EnsureInitialized();

 auto fields=PB::Parse((const uint8_t*)buf,len);

 if(isGetLastPlayed){
   if(!MetadataSync::syncPlaytime.load(std::memory_order_relaxed))
     return orig(t,m,buf,len,response,flags);
   const bool origResult=orig(t,m,buf,len,response,flags);
   if(!origResult) return origResult;
   auto res=StatsHandlers::HandleGetLastPlayedTimes(fields);
   if(response && res.body.Size() &&
      !ParseTypedMessage(response,res.body.Data()))
     return origResult;
   if(flags){ flags[2]=1; flags[3]=res.eresult; }
   LOG("[Mac] RPC handled: %s eresult=%d bytes=%zu",m,res.eresult,res.body.Size());
   return origResult;
 }

 const uint32_t app=ExtractRequestAppId(m,fields);
 if(!app || !CloudIntercept::IsNamespaceApp(app))
   return orig(t,m,buf,len,response,flags);

 if(isGetUserStats){
   if(!MetadataSync::syncAchievements.load(std::memory_order_relaxed))
     return orig(t,m,buf,len,response,flags);
   auto res=StatsHandlers::HandleGetUserStats(app,fields);
   if(response && res.body.Size() &&
      !ParseTypedMessage(response,res.body.Data()))
     return orig(t,m,buf,len,response,flags);
   if(flags){ flags[2]=1; flags[3]=res.eresult; }
   LOG("[Mac] RPC handled: %s app=%u eresult=%d bytes=%zu",
       m,app,res.eresult,res.body.Size());
   return true;
 }

 if(HttpServer::GetPort()==0 && RequiresLocalHttp(m)){
   LOG("[Mac] Cloud HTTP server unavailable; passing through %s app=%u",m,app);
   return orig(t,m,buf,len,response,flags);
 }

 const uint32_t account=CloudIntercept::GetAccountId();
 if(!account)
   return orig(t,m,buf,len,response,flags);

 LocalStorage::InitApp(account,app);
 LocalMetadataStore::InitApp(account,app);

 const bool isFileDownload =
     strcmp(m,CloudIntercept::RPC_FILE_DOWNLOAD)==0;
 if(isFileDownload){
   // Let Steam do its normal session/transport bookkeeping first, then
   // replace only the typed protobuf response with our local result.
   const bool origResult=orig(t,m,buf,len,response,flags);
   auto res=Dispatch(m,app,fields);
   if(!res.has_value())
     return origResult;
   if(response && res->body.Size() &&
      !ParseTypedMessage(response,res->body.Data()))
     return origResult;
   if(flags){ flags[2]=1; flags[3]=res->eresult; }
   LOG("[Mac] Cloud RPC handled: %s app=%u eresult=%d bytes=%zu",
       m,app,res->eresult,res->body.Size());
   return true;
 }

 auto res=Dispatch(m,app,fields);
 if(!res.has_value()){
   LOG("[Mac] Cloud RPC unhandled: %s app=%u -> native",m,app);
   return orig(t,m,buf,len,response,flags);
 }

 if(response && res->body.Size() &&
    !ParseTypedMessage(response,res->body.Data())){
   LOG("[Mac] Cloud RPC response parse failed: %s app=%u -> native",m,app);
   return orig(t,m,buf,len,response,flags);
 }
 if(flags){
   flags[2]=1;
   flags[3]=res->eresult;
 }
 LOG("[Mac] Cloud RPC handled: %s app=%u eresult=%d bytes=%zu",
     m,app,res->eresult,res->body.Size());
 return true;
}

extern "C" bool hook_NotificationDirect(
    void*t,const char*m,void*message,int*flags)
{
 HookGuard guard;
 auto orig=g_origServerNotification.load(std::memory_order_acquire);
 for(int i=0; !orig && i<1000; ++i){
   std::this_thread::sleep_for(std::chrono::microseconds(100));
   orig=g_origServerNotification.load(std::memory_order_acquire);
 }
 if(!orig || g_shuttingDown.load(std::memory_order_acquire) || !m)
   return orig ? orig(t,m,message,flags) : false;

 if(strncmp(m,"Cloud.",6)==0)
   LOG("[Mac] Cloud notification observed: %s",m);

 // Slot 7 is the notification ABI (protobuf object + flags), not the raw
 // request/response ABI. Keep it native until a safe message serializer is
 // proven for this Steam build.
 return orig(t,m,message,flags);
}

extern "C" bool hook_IsCloudEnabledForApp(void*t,unsigned int app){
 auto orig=g_origCloud.load(std::memory_order_acquire);
 if(CloudIntercept::IsNamespaceApp(app)) return true;
 return orig?orig(t,app):true;
}
extern "C" void CR_SetCrashContext(const char*,const char*,uint32_t){}
