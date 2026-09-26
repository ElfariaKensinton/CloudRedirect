#include "platform.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <filesystem>
#include <vector>

class MacPlatform: public IPlatform {
public:
 std::filesystem::path Utf8ToPath(const std::string& s) override { return std::filesystem::path(s); }
 std::string PathToUtf8(const std::filesystem::path& p) override { return p.string(); }
 std::string WideToUtf8(const wchar_t* w) override { return WideToUtf8(w,w?std::char_traits<wchar_t>::length(w):0); }
 std::string WideToUtf8(const wchar_t* w,size_t len) override {
  if(!w) return {};
  std::string r;
  for(size_t i=0;i<len;i++){ uint32_t cp=(uint32_t)w[i];
   if(cp<0x80) r.push_back((char)cp);
   else if(cp<0x800){r.push_back((char)(0xC0|(cp>>6)));r.push_back((char)(0x80|(cp&0x3F)));}
   else if(cp<0x10000){r.push_back((char)(0xE0|(cp>>12)));r.push_back((char)(0x80|((cp>>6)&0x3F)));r.push_back((char)(0x80|(cp&0x3F)));}
   else if(cp<0x110000){r.push_back((char)(0xF0|(cp>>18)));r.push_back((char)(0x80|((cp>>12)&0x3F)));r.push_back((char)(0x80|((cp>>6)&0x3F)));r.push_back((char)(0x80|(cp&0x3F)));}
  } return r;
 }
 bool AtomicWriteBinary(const std::string& path,const void* data,size_t len) override {
  static std::atomic<uint32_t> seq{0};
  std::string tmp=path+".tmp."+std::to_string(getpid())+"."+std::to_string(seq.fetch_add(1,std::memory_order_relaxed));
  int fd=open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600); if(fd<0)return false;
  const uint8_t* p=(const uint8_t*)data; size_t n=len;
  while(n){ssize_t w=write(fd,p,n);if(w<0){if(errno==EINTR)continue;close(fd);unlink(tmp.c_str());return false;}p+=w;n-=w;}
  if(fsync(fd)!=0){close(fd);unlink(tmp.c_str());return false;} close(fd);
  if(rename(tmp.c_str(),path.c_str())!=0){unlink(tmp.c_str());return false;} return true;
 }
 bool AtomicWriteText(const std::string& path,const std::string& c) override {return AtomicWriteBinary(path,c.data(),c.size());}
 bool IsPathWithin(const std::string& root,const std::string& full) override {
  std::error_code ec; auto a=std::filesystem::weakly_canonical(root,ec); if(ec)return false; auto b=std::filesystem::weakly_canonical(full,ec); if(ec)return false;
  const std::string as=a.string(), bs=b.string(); return bs==as || (bs.size()>as.size()&&bs.rfind(as+"/",0)==0);
 }
 void CleanupEmptyDirsUpTo(const std::string& start,const std::string& stop) override {
  std::error_code ec; auto cur=std::filesystem::weakly_canonical(start,ec); auto base=std::filesystem::weakly_canonical(stop,ec); if(ec)return;
  for(int i=0;i<256 && cur!=base;i++){ if(!std::filesystem::remove(cur,ec)||ec)break; cur=cur.parent_path(); }
 }
 bool IsPathRedirectingReparsePoint(const std::string& path) override {
  std::error_code ec;
  auto st = std::filesystem::symlink_status(std::filesystem::path(path), ec);
  return !ec && std::filesystem::is_symlink(st);
 }
 char PathSeparator() const override {return '/';}
 const char* PathSeparatorStr() const override {return "/";}
 std::string NormalizePath(const std::string& p) const override {return p;}
 std::vector<uint8_t> SHA1(const void* data,size_t len) override {
  uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0; uint64_t bits=(uint64_t)len*8;
  std::vector<uint8_t> m((const uint8_t*)data,(const uint8_t*)data+len);m.push_back(0x80);while(m.size()%64!=56)m.push_back(0);
  for(int i=7;i>=0;i--)m.push_back((uint8_t)(bits>>(i*8)));
  auto rot=[](uint32_t x,int n){return (x<<n)|(x>>(32-n));};
  for(size_t o=0;o<m.size();o+=64){uint32_t w[80];for(int i=0;i<16;i++)w[i]=((uint32_t)m[o+i*4]<<24)|((uint32_t)m[o+i*4+1]<<16)|((uint32_t)m[o+i*4+2]<<8)|m[o+i*4+3];for(int i=16;i<80;i++)w[i]=rot(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
   uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;for(int i=0;i<80;i++){uint32_t f,k;if(i<20){f=(b&c)|((~b)&d);k=0x5A827999;}else if(i<40){f=b^c^d;k=0x6ED9EBA1;}else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}else{f=b^c^d;k=0xCA62C1D6;}uint32_t t=rot(a,5)+f+e+k+w[i];e=d;d=c;c=rot(b,30);b=a;a=t;}h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;}
  std::vector<uint8_t> h(20);auto st=[&](int o,uint32_t v){h[o]=v>>24;h[o+1]=v>>16;h[o+2]=v>>8;h[o+3]=v;};st(0,h0);st(4,h1);st(8,h2);st(12,h3);st(16,h4);return h;
 }
};
static MacPlatform g_platform;
IPlatform& Platform(){return g_platform;}
