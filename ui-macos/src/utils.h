#pragma once
#include <QString>
#include <QDir>
#include <QFile>
#include <QStringList>

inline QString realHomePath() { return QDir::homePath(); }
inline bool inFlatpak() { return false; }
inline QString xdgConfigHome() { return realHomePath() + "/Library/Application Support"; }
inline QString xdgDataHome() { return realHomePath() + "/Library/Application Support"; }
inline QString crConfigDir() { return xdgConfigHome() + "/CloudRedirect"; }
inline QString crDataDir() { return xdgDataHome() + "/CloudRedirect"; }

inline QString steamDataPath()
{
    return realHomePath() + "/Library/Application Support/Steam";
}

inline QString steamAppBundle()
{
    const QString systemApp = "/Applications/Steam.app";
    if (QDir(systemApp).exists()) return systemApp;
    const QString userApp = realHomePath() + "/Applications/Steam.app";
    if (QDir(userApp).exists()) return userApp;
    return systemApp;
}

inline QString steamExecutable()
{
    const QStringList candidates = {
        steamAppBundle() + "/Contents/MacOS/steam_osx",
        steamAppBundle() + "/Contents/MacOS/steam",
        steamDataPath() + "/Steam.AppBundle/Steam/Contents/MacOS/steam_osx",
        steamDataPath() + "/Steam.AppBundle/Steam/Contents/MacOS/steam"
    };
    for (const QString &p : candidates)
        if (QFile::exists(p)) return p;
    return candidates.first();
}
