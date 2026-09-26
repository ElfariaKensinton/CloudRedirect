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
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
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

using Send4Fn=bool(*)(void*,const char*,void*,void*,int*);
using Send5Fn=bool(*)(void*,const char*,void*,void*,int*);
using NotifyFn=bool(*)(void*,const char*,void*,int*);
using Send8Fn=bool(*)(void*,const char*,void*,unsigned int,void*,int*);
using CloudEnabledFn=bool(*)(void*,unsigned int);

static std::atomic<Send4Fn> g_orig4{nullptr};
static std::atomic<Send5Fn> g_orig5{nullptr};
static std::atomic<NotifyFn> g_orig7{nullptr};
static std::atomic<Send8Fn> g_orig8{nullptr};
static std::atomic<CloudEnabledFn> g_origCloud{nullptr};
static std::atomic<bool> g_initialized{false},g_shuttingDown{false};
static std::atomic<int> g_hookRefCount{0};
static std::once_flag g_initOnce;
static std::atomic<bool> g_protoReady{false};
static std::atomic<bool> g_statsSyncEnabled{false};
static std::thread g_seedThread;
static std::thread g_cloudPollerThread;
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

using SerializeFn=bool(*)(const void*,void*,int);
using ParseFn=bool(*)(void*,const void*,int);
static SerializeFn g_serialize=nullptr; static ParseFn g_parse=nullptr;
using ByteSizeFn=size_t(*)(const void*);
static ByteSizeFn g_byteSize=nullptr;

static std::vector<uint8_t> SerializeMessage(void*msg){
 if(!msg||!g_serialize)return{};
 constexpr size_t kMax = 64ULL*1024ULL*1024ULL;
 if(g_byteSize){
   const size_t size=g_byteSize(msg);
   if(size==0||size>kMax||size>0x7fffffffULL)return{};
   std::vector<uint8_t> buf(size);
   if(!g_serialize(msg,buf.data(),static_cast<int>(size)))return{};
   return buf;
 }

 // Some macOS Steam builds do not export ByteSizeLong. Avoid guessing a
 // protobuf vtable index: grow a bounded buffer until SerializeToArray fits.
 for(size_t size=4096; size<=kMax; size*=2){
   std::vector<uint8_t> buf(size);
   if(g_serialize(msg,buf.data(),static_cast<int>(size)))
     return buf;
   if(size > kMax/2) break;
 }
 return {};
}
static bool ParseIntoMessage(void*msg,const uint8_t*d,size_t n){
 return msg&&d&&n&&g_parse&&n<=0x7fffffffULL&&g_parse(msg,d,(int)n);
}
static bool RequiresLocalHttp(const char* method)
{
 return method &&
   (strcmp(method,CloudIntercept::RPC_BEGIN_UPLOAD)==0 ||
    strcmp(method,CloudIntercept::RPC_FILE_DOWNLOAD)==0);
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
       g_seedThread=std::thread([]{
         if(!g_shuttingDown.load(std::memory_order_acquire))
           StatsStore::SeedApps(CloudIntercept::GetNamespaceApps());
         g_seedExited.store(true,std::memory_order_release);
         g_seedExitCv.notify_all();
       });

       g_pollerExited.store(false,std::memory_order_release);
       g_cloudPollerThread=std::thread([]{
         while(!g_shuttingDown.load(std::memory_order_acquire)){
           for(int i=0;i<60 && !g_shuttingDown.load(std::memory_order_acquire);++i)
             std::this_thread::sleep_for(std::chrono::seconds(1));
           if(g_shuttingDown.load(std::memory_order_acquire))break;
           const auto apps=CloudIntercept::GetNamespaceApps();
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
     StatsHooks::SetProtobufHelpers(SerializeMessage,ParseIntoMessage);
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
void SetOriginals(void*a,void*b,void*c,void*d){
 g_orig4.store((Send4Fn)a,std::memory_order_release);
 g_orig5.store((Send5Fn)b,std::memory_order_release);
 g_orig7.store((NotifyFn)c,std::memory_order_release);
 g_orig8.store((Send8Fn)d,std::memory_order_release);
}
void SetOriginalIsCloudEnabled(void*o){g_origCloud.store((CloudEnabledFn)o,std::memory_order_release);}
static void* ResolveLocalMachOSymbol(const char* imagePath, intptr_t slide, const char* symbol)
{
    if (!imagePath || !symbol) return nullptr;

    const int fd = open(imagePath, O_RDONLY);
    if (fd < 0) return nullptr;

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(mach_header_64))) {
        close(fd);
        return nullptr;
    }

    void* mapped = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) return nullptr;

    void* result = nullptr;
    const auto* header = static_cast<const mach_header_64*>(mapped);
    if (header->magic != MH_MAGIC_64) {
        munmap(mapped, static_cast<size_t>(st.st_size));
        return nullptr;
    }

    const auto* lc = reinterpret_cast<const load_command*>(
        reinterpret_cast<const uint8_t*>(header) + sizeof(mach_header_64));

    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (lc->cmd == LC_SYMTAB) {
            const auto* symtab = reinterpret_cast<const symtab_command*>(lc);
            const uint64_t symbolsEnd =
                static_cast<uint64_t>(symtab->symoff) +
                static_cast<uint64_t>(symtab->nsyms) * sizeof(nlist_64);
            const uint64_t stringsEnd =
                static_cast<uint64_t>(symtab->stroff) +
                static_cast<uint64_t>(symtab->strsize);

            if (symbolsEnd <= static_cast<uint64_t>(st.st_size) &&
                stringsEnd <= static_cast<uint64_t>(st.st_size)) {
                const auto* symbols = reinterpret_cast<const nlist_64*>(
                    reinterpret_cast<const uint8_t*>(mapped) + symtab->symoff);
                const char* strings =
                    reinterpret_cast<const char*>(reinterpret_cast<const uint8_t*>(mapped) + symtab->stroff);

                for (uint32_t n = 0; n < symtab->nsyms; ++n) {
                    const nlist_64& entry = symbols[n];
                    if (entry.n_un.n_strx == 0 || entry.n_un.n_strx >= symtab->strsize)
                        continue;
                    if ((entry.n_type & N_TYPE) != N_SECT)
                        continue;

                    const char* name = strings + entry.n_un.n_strx;
                    const size_t maxName = symtab->strsize - entry.n_un.n_strx;
                    const size_t nameLen = strnlen(name, maxName);
                    if (nameLen == maxName)
                        continue;

                    if (strcmp(name, symbol) == 0) {
                        result = reinterpret_cast<void*>(static_cast<uintptr_t>(entry.n_value) + slide);
                        break;
                    }
                }
            }
        }

        if (result || lc->cmdsize == 0)
            break;
        lc = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
    }

    munmap(mapped, static_cast<size_t>(st.st_size));
    return result;
}

static void* ResolveSteamclientSymbol(void* steamclientHandle, const char* symbol)
{
    if (steamclientHandle) {
        if (void* p = dlsym(steamclientHandle, symbol))
            return p;
    }
    return dlsym(RTLD_DEFAULT, symbol);
}

bool ResolveProtobufHelpers(void* steamclientBase,size_t){
 const char* ser="_ZNK6google8protobuf11MessageLite15SerializeToArrayEPvi";
 const char* par="_ZN6google8protobuf11MessageLite14ParseFromArrayEPKvi";
 const char* size="_ZNK6google8protobuf11MessageLite11ByteSizeLongEv";

 void* handle = nullptr;
 const char* imageName = nullptr;
 intptr_t imageSlide = 0;
 if (steamclientBase) {
     uint32_t count = _dyld_image_count();
     for (uint32_t i = 0; i < count; ++i) {
         const mach_header* h = _dyld_get_image_header(i);
         if (!h) continue;
         if (reinterpret_cast<uintptr_t>(h) != reinterpret_cast<uintptr_t>(steamclientBase)) continue;
         imageName = _dyld_get_image_name(i);
         imageSlide = _dyld_get_image_vmaddr_slide(i);
         if (imageName)
             handle = dlopen(imageName, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
         break;
     }
 }

 g_serialize=reinterpret_cast<SerializeFn>(ResolveSteamclientSymbol(handle,ser));
 g_parse=reinterpret_cast<ParseFn>(ResolveSteamclientSymbol(handle,par));
 g_byteSize=reinterpret_cast<ByteSizeFn>(ResolveSteamclientSymbol(handle,size));

 if ((!g_serialize || !g_parse) && imageName) {
     if (!g_serialize)
         g_serialize=reinterpret_cast<SerializeFn>(ResolveLocalMachOSymbol(imageName, imageSlide, ser));
     if (!g_parse)
         g_parse=reinterpret_cast<ParseFn>(ResolveLocalMachOSymbol(imageName, imageSlide, par));
     if (!g_byteSize)
         g_byteSize=reinterpret_cast<ByteSizeFn>(ResolveLocalMachOSymbol(imageName, imageSlide, size));
 }

 if (handle) dlclose(handle);

 g_protoReady.store(g_serialize&&g_parse,std::memory_order_release);
 if(g_protoReady.load(std::memory_order_acquire))
   StatsHooks::SetProtobufHelpers(SerializeMessage,ParseIntoMessage);
 LOG("[Mac] protobuf helpers %s (serialize=%p parse=%p size=%p)",
     g_protoReady.load()?"resolved":"incomplete; cloud RPC hooks will pass through",
     (void*)g_serialize,(void*)g_parse,(void*)g_byteSize);
 return g_protoReady.load(std::memory_order_acquire);
}
void InstallGamesPlayedObserver(uintptr_t,size_t){
 LOG("[Mac] GamesPlayed observer unavailable on this build; playtime uses native stats reconciliation/poller");
}
void BeginShutdown(){
 g_shuttingDown.store(true,std::memory_order_release);
 HttpServer::Stop();

 // Stop background stats work before tearing down CloudStorage.
 if(g_seedThread.joinable()){
   std::unique_lock<std::mutex> lk(g_seedExitMtx);
   g_seedExitCv.wait(lk,[] { return g_seedExited.load(std::memory_order_acquire); });
   lk.unlock();
   g_seedThread.join();
 }
 if(g_cloudPollerThread.joinable()){
   std::unique_lock<std::mutex> lk(g_pollerExitMtx);
   g_pollerExitCv.wait(lk,[] { return g_pollerExited.load(std::memory_order_acquire); });
   lk.unlock();
   g_cloudPollerThread.join();
 }
 if(g_statsSyncEnabled.load(std::memory_order_acquire)) {
   if(MetadataSync::syncPlaytime.load(std::memory_order_relaxed))
     StatsStore::RefreshLocalPlaytime();
   StatsHandlers::Shutdown();
 }

 // No Steam thread may still be inside a hook before CloudStorage is destroyed.
 while(g_hookRefCount.load(std::memory_order_acquire)>0)
   std::this_thread::sleep_for(std::chrono::milliseconds(1));

 CloudStorage::Shutdown();
}
}
static uint32_t CheckNotificationNamespaceApp(const char* methodName, void* body)
{
 if(!methodName||!body||!g_protoReady.load(std::memory_order_acquire))return 0;
 auto bytes=SerializeMessage(body);
 if(bytes.empty())return 0;
 auto fields=PB::Parse(bytes.data(),bytes.size());
 auto* appField=PB::FindField(fields,1);
 if(!appField)return 0;
 uint32_t app=(uint32_t)appField->varintVal;
 return app && CloudIntercept::IsNamespaceApp(app) ? app : 0;
}

extern "C" bool hook_ServiceMethodDirect(void*t,const char*m,void*r,void*s,int*f)
{
 HookGuard guard;
 auto orig=g_orig4.load(std::memory_order_acquire);
 if(!orig||g_shuttingDown.load(std::memory_order_acquire)||!m)
   return orig?orig(t,m,r,s,f):false;
 if(!g_protoReady.load(std::memory_order_acquire))
   return orig(t,m,r,s,f);

 if(strcmp(m,StatsHandlers::RPC_GET_USER_STATS)==0 &&
    MetadataSync::syncAchievements.load(std::memory_order_relaxed)) {
   EnsureInitialized();
   if(StatsHooks::TryHandleGetUserStats(m,r,s,f)) return true;
   return orig(t,m,r,s,f);
 }

 if(strncmp(m,"Cloud.",6)!=0)
   return orig(t,m,r,s,f);

 const bool directCloudRpc =
   strcmp(m,CloudIntercept::RPC_BEGIN_UPLOAD)==0 ||
   strcmp(m,CloudIntercept::RPC_COMMIT_UPLOAD)==0 ||
   strcmp(m,CloudIntercept::RPC_FILE_DOWNLOAD)==0 ||
   strcmp(m,CloudIntercept::RPC_DELETE_FILE)==0;
 if(!directCloudRpc)
   return orig(t,m,r,s,f);

 EnsureInitialized();
 if(HttpServer::GetPort()==0 && RequiresLocalHttp(m))
   return orig(t,m,r,s,f);

 auto bytes=SerializeMessage(r);
 if(bytes.empty())
   return orig(t,m,r,s,f);
 auto fields=PB::Parse(bytes.data(),bytes.size());
 uint32_t app=CloudRpcUtils::ExtractAppId(m,fields);
 if(!app||!CloudIntercept::IsNamespaceApp(app))
   return orig(t,m,r,s,f);

 uint32_t account=CloudIntercept::GetAccountId();
 if(!account)
   return orig(t,m,r,s,f);

 LocalStorage::InitApp(account,app);
 LocalMetadataStore::InitApp(account,app);

 auto res=Dispatch(m,app,fields);
 if(!res.has_value())
   return orig(t,m,r,s,f);

 if(res->body.Size()>0) {
   if(!s || !ParseIntoMessage(s,res->body.Data().data(),res->body.Size()))
     return false;
 }
 if(f) {
   f[2]=1;
   f[3]=res->eresult;
 }
 return true;
}

extern "C" bool hook_BYieldingSend(void*t,const char*m,void*r,void*s,int*f){
 HookGuard guard;
 auto orig=g_orig5.load(); if(g_shuttingDown.load(std::memory_order_acquire)||!m||!orig)return orig?orig(t,m,r,s,f):0;
 if(!g_protoReady.load())return orig(t,m,r,s,f);
 if(strcmp(m,StatsHandlers::RPC_GET_USER_STATS)==0 && MetadataSync::syncAchievements.load()){
   EnsureInitialized();
   if(StatsHooks::TryHandleGetUserStats(m,r,s,f)) return 1;
   return orig(t,m,r,s,f);
 }
 if(strcmp(m,StatsHandlers::RPC_GET_LAST_PLAYED)==0 && MetadataSync::syncPlaytime.load()){
   EnsureInitialized();
   const int rc=orig(t,m,r,s,f);
   if(rc) StatsHooks::MergeLastPlayedTimes(m,r,s);
   return rc;
 }
 if(strncmp(m,"Cloud.",6)!=0)return orig(t,m,r,s,f);
 if(strcmp(m,CloudIntercept::RPC_FILE_DOWNLOAD)==0){
   EnsureInitialized();
   auto bytes=SerializeMessage(r);
   if(bytes.empty())return orig(t,m,r,s,f);
   auto fields=PB::Parse(bytes.data(),bytes.size());
   uint32_t app=CloudRpcUtils::ExtractAppId(m,fields);
   if(!app||!CloudIntercept::IsNamespaceApp(app))return orig(t,m,r,s,f);
 if(HttpServer::GetPort()==0 && RequiresLocalHttp(m)){
   LOG("[Mac] Cloud HTTP server unavailable; passing through %s for app %u",m,app);
   return orig(t,m,r,s,f);
 }
   const int origResult=orig(t,m,r,s,f);
   auto res=Dispatch(m,app,fields);
   if(!res.has_value()||!s||res->body.Size()==0)return origResult;
   if(!ParseIntoMessage(s,res->body.Data().data(),res->body.Size()))return origResult;
   if(f){f[2]=1;f[3]=res->eresult;}
   return 1;
 }
 EnsureInitialized(); auto bytes=SerializeMessage(r); if(bytes.empty())return orig(t,m,r,s,f);
 auto fields=PB::Parse(bytes.data(),bytes.size()); uint32_t app=CloudRpcUtils::ExtractAppId(m,fields);
 if(!app||!CloudIntercept::IsNamespaceApp(app))return orig(t,m,r,s,f);
 if(HttpServer::GetPort()==0 && RequiresLocalHttp(m)){
   LOG("[Mac] Cloud HTTP server unavailable; passing through %s for app %u",m,app);
   return orig(t,m,r,s,f);
 }
 auto account=CloudIntercept::GetAccountId(); if(!account)return orig(t,m,r,s,f);
 LocalStorage::InitApp(account,app); LocalMetadataStore::InitApp(account,app);
 auto res=Dispatch(m,app,fields); if(!res.has_value())return orig(t,m,r,s,f);
 if(s&&res->body.Size()&&ParseIntoMessage(s,res->body.Data().data(),res->body.Size())){if(f){f[2]=1;f[3]=res->eresult;}return 1;}
 return orig(t,m,r,s,f);
}
extern "C" bool hook_NotificationDirect(void*t,const char*m,void*b,int*f){
 HookGuard guard;
 auto orig=g_orig7.load();
 if(g_shuttingDown.load(std::memory_order_acquire)||!m||!orig)return orig?orig(t,m,b,f):0;
 if(!g_protoReady.load(std::memory_order_acquire)||strncmp(m,"Cloud.",6)!=0)
   return orig(t,m,b,f);

 uint32_t app=CheckNotificationNamespaceApp(m,b);
 if(!app)return orig(t,m,b,f);
 if(HttpServer::GetPort()==0)return orig(t,m,b,f);

 if(strcmp(m,CloudIntercept::RPC_EXIT_SYNC)==0){
   auto bytes=SerializeMessage(b);
   if(!bytes.empty()){
     auto fields=PB::Parse(bytes.data(),bytes.size());
     uint64_t clientId=0;
     bool uploadsCompleted=false, uploadsRequired=false;
     if(auto* x=PB::FindField(fields,2))clientId=x->varintVal;
     if(auto* x=PB::FindField(fields,3))uploadsCompleted=x->varintVal!=0;
     if(auto* x=PB::FindField(fields,4))uploadsRequired=x->varintVal!=0;
     uint32_t account=CloudIntercept::GetAccountId();
     if(account){
       PendingOpsJournal::RecordExitSyncState(account,app,uploadsCompleted,uploadsRequired,clientId);
       std::thread([account,app,clientId]{
         CloudStorage::InflightSyncScope guard;
         if(!guard.entered)return;
         CloudStorage::ReleaseCloudSession(account,app,clientId);
       }).detach();
     }
   }
   return orig(t,m,b,f);
 }

 if(strcmp(m,CloudIntercept::RPC_CONFLICT)==0){
   auto bytes=SerializeMessage(b);
   if(!bytes.empty()){
     auto fields=PB::Parse(bytes.data(),bytes.size());
     bool choseLocal=false;
     if(auto* x=PB::FindField(fields,2))choseLocal=x->varintVal!=0;
     CloudIntercept::RecordConflictResolution(app,choseLocal);
   }
 }

 // Cloud notifications for namespace apps describe the real Steam backend.
 // Our local RPC path is authoritative, so forwarding the notification would
 // re-apply remote state and race the redirect's local metadata.
 return 1;
}
extern "C" bool hook_SyncSend2(void*t,const char*m,void*buf,unsigned int len,void*r,int*f)
{
 HookGuard guard;
 auto orig=g_orig8.load(std::memory_order_acquire);
 if(g_shuttingDown.load(std::memory_order_acquire)||!orig)
   return orig?orig(t,m,buf,len,r,f):false;
 if(!m||strncmp(m,"Cloud.",6)!=0||!buf||!len)
   return orig(t,m,buf,len,r,f);

 EnsureInitialized();
 auto fields=PB::Parse((const uint8_t*)buf,len);
 uint32_t app=CloudRpcUtils::ExtractAppId(m,fields);
 if(!app||!CloudIntercept::IsNamespaceApp(app))
   return orig(t,m,buf,len,r,f);

 if(HttpServer::GetPort()==0 && RequiresLocalHttp(m))
   return orig(t,m,buf,len,r,f);

 uint32_t account=CloudIntercept::GetAccountId();
 if(!account)
   return orig(t,m,buf,len,r,f);

 LocalStorage::InitApp(account,app);
 LocalMetadataStore::InitApp(account,app);
 auto res=Dispatch(m,app,fields);
 if(!res.has_value())
   return orig(t,m,buf,len,r,f);

 if(r&&res->body.Size()){
   if(!ParseIntoMessage(r,res->body.Data().data(),res->body.Size()))
     return orig(t,m,buf,len,r,f);
 }
 if(f){
   f[2]=1;
   f[3]=res->eresult;
 }
 return true;
}

extern "C" bool hook_IsCloudEnabledForApp(void*t,unsigned int app){
 auto orig=g_origCloud.load(); if(CloudIntercept::IsNamespaceApp(app))return true; return orig?orig(t,app):true;
}
extern "C" void CR_SetCrashContext(const char*,const char*,uint32_t){}
