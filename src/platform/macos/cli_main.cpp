#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using CliMainFn=int(*)(int,char**);
static void resolveDylibPath(char*out,size_t n){
 uint32_t size=(uint32_t)n; uint32_t cap=size;
 if(_NSGetExecutablePath(out,&cap)==0){
   char* slash=strrchr(out,'/'); if(slash){slash[1]='\0';strncat(out,"cloud_redirect.dylib",n-strlen(out)-1);return;}
 }
 snprintf(out,n,"cloud_redirect.dylib");
}
int main(int argc,char**argv){
 char path[PATH_MAX]={}; resolveDylibPath(path,sizeof(path));
 void* h=dlopen(path,RTLD_NOW|RTLD_LOCAL); if(!h)h=dlopen("cloud_redirect.dylib",RTLD_NOW|RTLD_LOCAL);
 if(!h){fprintf(stderr,"Error: cannot load CloudRedirect dylib: %s\n",dlerror());return 1;}
 dlerror(); auto fn=(CliMainFn)dlsym(h,"CloudRedirect_CliMain"); const char*e=dlerror();
 if(!fn||e){fprintf(stderr,"Error: cannot find CloudRedirect_CliMain: %s\n",e?e:"unknown");dlclose(h);return 1;}
 int rc; if(argc>=2&&strcmp(argv[1],"--cli")==0) rc=fn(argc,argv);
 else {char**a=(char**)malloc((argc+2)*sizeof(char*));a[0]=argv[0];a[1]=(char*)"--cli";for(int i=1;i<argc;i++)a[i+1]=argv[i];a[argc+1]=nullptr;rc=fn(argc+1,a);free(a);}
 dlclose(h); return rc;
}