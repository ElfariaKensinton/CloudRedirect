#include "vtable_hook.h"
#include "cloud_hooks.h"
#include "log.h"
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <algorithm>

struct Range { uintptr_t start=0,end=0; vm_prot_t prot=0; };
static std::vector<Range> g_ranges;
static bool g_ready=false;

static bool Contains(uintptr_t a,size_t n,const Range&r){return a>=r.start&&a+n<=r.end;}
static bool InReadableRanges(uintptr_t a,size_t n){for(const auto&r:g_ranges)if(Contains(a,n,r))return true;return false;}
static bool InExecutableRanges(uintptr_t a,size_t n)
{
    for(const auto&r:g_ranges)
        if((r.prot & VM_PROT_EXECUTE) && Contains(a,n,r)) return true;
    return false;
}
static void CollectRanges(const struct mach_header_64* h,intptr_t slide,size_t& span){
    g_ranges.clear(); span=0;
    uintptr_t image=(uintptr_t)h, minA=UINTPTR_MAX,maxA=0;
    const load_command* lc=(const load_command*)((const uint8_t*)h+sizeof(mach_header_64));
    for(uint32_t i=0;i<h->ncmds;i++){
        if(lc->cmd==LC_SEGMENT_64){
            const segment_command_64* s=(const segment_command_64*)lc;
            uintptr_t a=(uintptr_t)(s->vmaddr+slide), e=a+(uintptr_t)s->vmsize;
            minA=std::min(minA,a); maxA=std::max(maxA,e);
            if(s->initprot&VM_PROT_READ) g_ranges.push_back({a,e,s->initprot});
        }
        lc=(const load_command*)((const uint8_t*)lc+lc->cmdsize);
    }
    if(minA!=UINTPTR_MAX) {span=maxA-minA;}
}
uintptr_t VtableHook::FindSteamclient(size_t& outSize){
    uint32_t n=_dyld_image_count();
    for(uint32_t i=0;i<n;i++){
        const char* name=_dyld_get_image_name(i); if(!name)continue;
        if(strstr(name,"steamclient")||strstr(name,"SteamClient")){
            intptr_t slide=_dyld_get_image_vmaddr_slide(i);
            auto* h=(const mach_header_64*)_dyld_get_image_header(i); if(!h||h->magic!=MH_MAGIC_64)continue;
            CollectRanges(h,slide,outSize); g_ready=true; return (uintptr_t)h;
        }
    }
    return 0;
}
static const uint8_t* FindBytes(const void* bytes,size_t len,uintptr_t base,size_t size){
    for(const auto&r:g_ranges){uintptr_t s=std::max(r.start,base),e=std::min(r.end,base+size);if(e<=s||e-s<len)continue;
        const uint8_t* p=(const uint8_t*)s; for(uintptr_t a=s;a+len<=e;a++,p++) if(memcmp(p,bytes,len)==0)return p;}
    return nullptr;
}
static uintptr_t* FindPointer(uintptr_t value,uintptr_t base,size_t size){
    for(const auto&r:g_ranges){uintptr_t s=std::max(r.start,base),e=std::min(r.end,base+size);for(uintptr_t a=(s+7)&~7ULL;a+8<=e;a+=8){auto*p=(uintptr_t*)a;if(*p==value)return p;}}
    return nullptr;
}
void** VtableHook::FindVtableByRTTIName(const char* name,uintptr_t base,size_t size){
    if(!g_ready) return nullptr;
    const uint8_t* str=FindBytes(name,strlen(name)+1,base,size); if(!str){Log::Error("macOS RTTI string not found: %s",name);return nullptr;}
    uintptr_t strA=(uintptr_t)str; uintptr_t* nf=FindPointer(strA,base,size); if(!nf) {Log::Error("macOS RTTI typeinfo name pointer not found: %s",name);return nullptr;}
    uintptr_t ti=(uintptr_t)(nf-1);
    for(const auto&r:g_ranges){uintptr_t s=std::max(r.start,base),e=std::min(r.end,base+size);for(uintptr_t a=(s+7)&~7ULL;a+16<=e;a+=8){
        uintptr_t*p=(uintptr_t*)a; if(p[0]==0&&p[1]==ti){void** vt=(void**)(p+2);if(Contains((uintptr_t)vt,8,r)){Log::Info("macOS vtable %s @ %p",name,vt);return vt;}}
    }}
    return nullptr;
}
void** VtableHook::FindTransportVtable(uintptr_t b,size_t s){
    void** vt = FindVtableByRTTIName("30CClientUnifiedServiceTransport",b,s);
    if(!vt) return nullptr;
    if(!InReadableRanges((uintptr_t)vt,9*sizeof(void*)))
        return nullptr;
    for(int slot : {4,5,7,8}){
        uintptr_t fn=(uintptr_t)vt[slot];
        if(!InExecutableRanges(fn,1)){
            Log::Error("macOS transport vtable slot %d does not point into executable steamclient memory: %p",
                       slot,(void*)fn);
            return nullptr;
        }
    }
    return vt;
}
void** VtableHook::FindRemoteStorageVtable(uintptr_t b,size_t s){return FindVtableByRTTIName("18CUserRemoteStorage",b,s);}
void* VtableHook::FindGlobalWithVtable(void*v,uintptr_t b,size_t s){uintptr_t t=(uintptr_t)v;for(const auto&r:g_ranges){uintptr_t st=std::max(r.start,b),en=std::min(r.end,b+s);if(!(r.prot&VM_PROT_WRITE))continue;for(uintptr_t a=(st+7)&~7ULL;a+24<=en;a+=8){uintptr_t*p=(uintptr_t*)a;if(p[0]==t&&p[1]==0&&p[2]==0)return (void*)p;}}return nullptr;}
static bool MakeWritable(void*addr,size_t len){
    uintptr_t ps=(uintptr_t)getpagesize(),a=(uintptr_t)addr&~(ps-1),e=((uintptr_t)addr+len+ps-1)&~(ps-1);
    return mprotect((void*)a,e-a,PROT_READ|PROT_WRITE)==0;
}
static vm_prot_t QueryProtection(void*addr){
    mach_vm_address_t region=(mach_vm_address_t)addr;
    mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object=0;
    if(mach_vm_region(mach_task_self(),&region,&size,VM_REGION_BASIC_INFO_64,
                      (vm_region_info_t)&info,&count,&object)!=KERN_SUCCESS)
        return VM_PROT_READ;
    return info.protection;
}
static bool RestoreProtection(void*addr,size_t len,vm_prot_t prot){
    uintptr_t ps=(uintptr_t)getpagesize(),a=(uintptr_t)addr&~(ps-1),e=((uintptr_t)addr+len+ps-1)&~(ps-1);
    return mprotect((void*)a,e-a,prot)==0;
}
bool VtableHook::InstallHooks(void**vt,VtableInfo&i){
    if(!vt)return false;
    i.vtable=vt;
    i.origSlot4=vt[4];
    i.origSlot5=vt[5];
    i.origSlot6=vt[6];
    i.origSlot7=vt[7];
    i.origSlot8=vt[8];
    i.originalProtection=(int)QueryProtection(&vt[7]);

    // Keep typed slots 4/5 native on this Steam macOS build. Their ABI has
    // changed across builds and even a pass-through probe can corrupt the
    // caller if the signature is wrong. Slot 7 is a verified notification ABI;
    // slot 8 remains an observed raw SyncSend2 probe.
    const size_t slotCount=2*sizeof(void*);
    void** firstSlot=&vt[7];
    if(!MakeWritable(firstSlot,slotCount))return false;
    vt[7]=(void*)&hook_NotificationDirect;
    vt[8]=(void*)&hook_SyncSend2;

    const vm_prot_t originalProt=
        (vm_prot_t)(i.originalProtection?i.originalProtection:VM_PROT_READ);
    if(!RestoreProtection(firstSlot,slotCount,originalProt)){
        Log::Error("macOS transport hook: failed to restore vtable page protection; rolling back");
        if(MakeWritable(firstSlot,slotCount)){
            vt[7]=i.origSlot7;
            vt[8]=i.origSlot8;
            RestoreProtection(firstSlot,slotCount,originalProt);
        }
        i.vtable=nullptr;
        return false;
    }

    i.typedInstalled=false;
    Log::Info("macOS transport hooks installed (slot 7 notification + slot 8 probe; slots 4/5 native)");
    CloudHooks::SetOriginalRaw(i.origSlot8,i.origSlot7);
    return true;
}

bool VtableHook::InstallCloudEnabledHook(void**vt,CloudEnabledHookInfo& info){
    if(!vt) return false;
    constexpr size_t kAccountSlot = 18; // IsCloudEnabledForAccount
    constexpr size_t kAppSlot = 19;     // IsCloudEnabledForApp
    constexpr size_t kSetAppSlot = 20;  // SetCloudEnabledForApp
    constexpr size_t kSetAccountSlot = 61; // SetCloudEnabledForAccount

    if(!InReadableRanges((uintptr_t)vt,62*sizeof(void*)))
        return false;
    for(size_t slot : {kAccountSlot,kAppSlot,kSetAppSlot,kSetAccountSlot}){
        if(!InExecutableRanges((uintptr_t)vt[slot],1)){
            Log::Error("[Mac] RemoteStorage slot %zu is not executable: %p",slot,vt[slot]);
            return false;
        }
    }

    info.vtable=vt;
    info.origAccountSlot=vt[kAccountSlot];
    info.origAppSlot=vt[kAppSlot];
    info.origSetAppSlot=vt[kSetAppSlot];
    info.origSetAccountSlot=vt[kSetAccountSlot];
    info.accountSlotIndex=kAccountSlot;
    info.appSlotIndex=kAppSlot;
    info.setAppSlotIndex=kSetAppSlot;
    info.setAccountSlotIndex=kSetAccountSlot;

    void** first=&vt[kAccountSlot];
    if(!MakeWritable(first,3*sizeof(void*)))return false;
    vt[kAccountSlot]=(void*)&hook_IsCloudEnabledForAccount;
    vt[kAppSlot]=(void*)&hook_IsCloudEnabledForApp;
    vt[kSetAppSlot]=(void*)&hook_SetCloudEnabledForApp;
    const vm_prot_t prot=(vm_prot_t)(QueryProtection(first)?QueryProtection(first):VM_PROT_READ);
    if(!RestoreProtection(first,3*sizeof(void*),prot)){
        if(MakeWritable(first,3*sizeof(void*))){
            vt[kAccountSlot]=info.origAccountSlot;
            vt[kAppSlot]=info.origAppSlot;
            vt[kSetAppSlot]=info.origSetAppSlot;
            RestoreProtection(first,3*sizeof(void*),prot);
        }
        info={};
        return false;
    }

    void** setAccount=&vt[kSetAccountSlot];
    if(!MakeWritable(setAccount,sizeof(void*))){
        Log::Error("[Mac] unable to patch RemoteStorage SetCloudEnabledForAccount");
        if(MakeWritable(first,3*sizeof(void*))){
            vt[kAccountSlot]=info.origAccountSlot;
            vt[kAppSlot]=info.origAppSlot;
            vt[kSetAppSlot]=info.origSetAppSlot;
            RestoreProtection(first,3*sizeof(void*),prot);
        }
        info={};
        return false;
    }
    const vm_prot_t setProt=(vm_prot_t)(QueryProtection(setAccount)?QueryProtection(setAccount):VM_PROT_READ);
    info.origSetAccountSlot=vt[kSetAccountSlot];
    vt[kSetAccountSlot]=(void*)&hook_SetCloudEnabledForAccount;
    if(!RestoreProtection(setAccount,sizeof(void*),setProt)){
        if(MakeWritable(setAccount,sizeof(void*))){
            vt[kSetAccountSlot]=info.origSetAccountSlot;
            RestoreProtection(setAccount,sizeof(void*),setProt);
        }
        if(MakeWritable(first,3*sizeof(void*))){
            vt[kAccountSlot]=info.origAccountSlot;
            vt[kAppSlot]=info.origAppSlot;
            vt[kSetAppSlot]=info.origSetAppSlot;
            RestoreProtection(first,3*sizeof(void*),prot);
        }
        info={};
        return false;
    }

    CloudHooks::SetOriginalIsCloudEnabled(info.origAppSlot);
    CloudHooks::SetOriginalIsCloudEnabledAccount(info.origAccountSlot);
    CloudHooks::SetOriginalSetCloudEnabledApp(info.origSetAppSlot);
    CloudHooks::SetOriginalSetCloudEnabledAccount(info.origSetAccountSlot);
    Log::Info("macOS RemoteStorage Cloud state forced ON (slots %zu/%zu/%zu/%zu)",
              kAccountSlot,kAppSlot,kSetAppSlot,kSetAccountSlot);
    return true;
}

void VtableHook::RemoveHooks(const VtableInfo&i){
    if(!i.vtable)return;
    void** firstSlot = i.typedInstalled ? &i.vtable[4] : &i.vtable[7];
    const size_t slotCount = i.typedInstalled ? 5*sizeof(void*) : 2*sizeof(void*);
    if(MakeWritable(firstSlot,slotCount)){
        if(i.typedInstalled){
            i.vtable[4]=i.origSlot4;
            i.vtable[5]=i.origSlot5;
        }
        i.vtable[7]=i.origSlot7;
        i.vtable[8]=i.origSlot8;
        RestoreProtection(firstSlot,slotCount,
                          (vm_prot_t)(i.originalProtection?i.originalProtection:VM_PROT_READ));
    }
}
void VtableHook::RemoveCloudEnabledHook(const CloudEnabledHookInfo& info){
    if(!info.vtable)return;
    constexpr size_t kAccountSlot=18;
    constexpr size_t kAppSlot=19;
    constexpr size_t kSetAppSlot=20;
    constexpr size_t kSetAccountSlot=61;
    void** first=&info.vtable[kAccountSlot];
    if(MakeWritable(first,3*sizeof(void*))){
        info.vtable[kAccountSlot]=info.origAccountSlot;
        info.vtable[kAppSlot]=info.origAppSlot;
        info.vtable[kSetAppSlot]=info.origSetAppSlot;
        RestoreProtection(first,3*sizeof(void*),VM_PROT_READ);
    }
    void** setAccount=&info.vtable[kSetAccountSlot];
    if(MakeWritable(setAccount,sizeof(void*))){
        info.vtable[kSetAccountSlot]=info.origSetAccountSlot;
        RestoreProtection(setAccount,sizeof(void*),VM_PROT_READ);
    }
}
