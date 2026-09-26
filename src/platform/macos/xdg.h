#pragma once
#include <cstdlib>
#include <string>
#include <pwd.h>
#include <unistd.h>

inline bool InFlatpak() { return false; }
inline std::string XdgHome() {
    const char* home=getenv("HOME");
    if(home&&home[0]) return home;
    struct passwd* pw=getpwuid(getuid());
    return (pw&&pw->pw_dir)?pw->pw_dir:"/tmp";
}
inline std::string XdgConfigHome() { return XdgHome()+"/Library/Application Support"; }
inline std::string XdgDataHome() { return XdgHome()+"/Library/Application Support"; }
