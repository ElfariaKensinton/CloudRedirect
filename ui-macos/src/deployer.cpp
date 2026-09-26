#include "deployer.h"
#include "utils.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

static QString getDylibVersion(const QString &path)
{
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data=f.readAll(), marker="CloudRedirectVersion/";
    qsizetype pos=data.indexOf(marker); if(pos<0) return {};
    qsizetype start=pos+marker.size(), end=data.indexOf('\0',start);
    return QString::fromLatin1(data.mid(start,end<0?64:end-start)).section('+',0,0);
}
static bool steamAllowsDyldInjection(const QString &exe, QString &reason)
{
    QProcess p;
    p.start("/usr/bin/codesign", {"-d", "--verbose=4", exe});
    if (!p.waitForFinished(5000)) {
        reason = "Timed out while checking Steam's code signature.";
        return false;
    }
    const QByteArray diag = p.readAllStandardError();
    if (p.exitCode() != 0) {
        reason.clear();
        return true;
    }

    const QString text = QString::fromUtf8(diag);
    const QRegularExpression flagsRe("flags=0x([0-9a-fA-F]+)\\(");
    const auto match = flagsRe.match(text);
    bool hardened = false;
    if (match.hasMatch()) {
        bool ok = false;
        const quint32 flags = match.captured(1).toUInt(&ok, 16);
        hardened = ok && (flags & 0x10000u) != 0;
    }
    if (!hardened) {
        reason.clear();
        return true;
    }

    QProcess e;
    e.start("/usr/bin/codesign", {"-d", "--entitlements", ":-", exe});
    if (!e.waitForFinished(5000)) {
        reason = "Timed out while checking Steam's runtime entitlements.";
        return false;
    }
    const QString entitlements = QString::fromUtf8(e.readAllStandardError()) +
                                 QString::fromUtf8(e.readAllStandardOutput());

    if (!entitlements.contains("com.apple.security.cs.allow-dyld-environment-variables")) {
        reason = "Steam uses Hardened Runtime without "
                 "com.apple.security.cs.allow-dyld-environment-variables; "
                 "DYLD_INSERT_LIBRARIES will be ignored.";
        return false;
    }
    if (!entitlements.contains("com.apple.security.cs.disable-library-validation")) {
        reason = "Steam uses Library Validation without "
                 "com.apple.security.cs.disable-library-validation; "
                 "the external CloudRedirect dylib may be rejected.";
        return false;
    }
    reason.clear();
    return true;
}

static bool readSlssteamStatus(bool &installed, bool &cloudBlocked)
{
    installed = false;
    cloudBlocked = false;
    const QStringList candidates = {
        realHomePath() + "/.config/SLSsteam/config.yaml",
        realHomePath() + "/Library/Application Support/Steam/SLSsteam/config.yaml",
        realHomePath() + "/Library/Application Support/SLSsteam/config.yaml"
    };

    for (const QString &path : candidates) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        installed = true;
        const QString text = QString::fromUtf8(f.readAll());
        static const QRegularExpression disableRe(
            "^\\s*DisableCloud\\s*:\\s*(yes|true|1)\\s*(?:#.*)?$",
            QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::MultilineOption);
        cloudBlocked = disableRe.match(text).hasMatch();
        return true;
    }
    return false;
}

Deployer::Deployer(QObject *p):QObject(p){checkPrerequisites();}
bool Deployer::slssteamInstalled()const{return m_slssteamInstalled;} bool Deployer::headcrabInstalled()const{return m_headcrabInstalled;}
bool Deployer::alreadyDeployed()const{return m_alreadyDeployed;} bool Deployer::updateAvailable()const{return m_updateAvailable;}
bool Deployer::slsCloudBlocked()const{return m_slsCloudBlocked;} QString Deployer::statusMessage()const{return m_statusMessage;}
QString Deployer::bundledVersion()const{return m_bundledVersion;} QString Deployer::deployedVersion()const{return m_deployedVersion;}
QString Deployer::steamPath()const{return m_steamPath;} bool Deployer::steamRunning()const{return m_steamRunning;}

bool Deployer::detectSteamRunning()const{
    for (const QString &name : {QStringLiteral("steam_osx"), QStringLiteral("steam")}) {
        QProcess p;
        p.start("/usr/bin/pgrep", {"-x", name});
        if (!p.waitForFinished(1500)) continue;
        if (p.exitCode() == 0) return true;
    }
    return false;
}
void Deployer::checkPrerequisites(){
    m_steamPath=steamAppBundle(); m_steamRunning=detectSteamRunning();
    readSlssteamStatus(m_slssteamInstalled, m_slsCloudBlocked);
    const QString appDir=QCoreApplication::applicationDirPath();
    m_soDeployPath=crDataDir()+"/cloud_redirect.dylib"; m_cliDeployPath=crDataDir()+"/cloud_redirect_cli";
    const QStringList ds={QProcessEnvironment::systemEnvironment().value("CR_BUNDLED_DYLIB"),appDir+"/cloud_redirect.dylib",appDir+"/../Resources/cloud_redirect.dylib",appDir+"/../share/cloud_redirect/cloud_redirect.dylib"};
    const QStringList cs={QProcessEnvironment::systemEnvironment().value("CR_BUNDLED_CLI"),appDir+"/cloud_redirect_cli",appDir+"/../Resources/cloud_redirect_cli",appDir+"/../share/cloud_redirect/cloud_redirect_cli"};
    for(const auto&p:ds)if(!p.isEmpty()&&QFile::exists(p)){m_soSourcePath=QFileInfo(p).canonicalFilePath();break;}
    for(const auto&p:cs)if(!p.isEmpty()&&QFile::exists(p)){m_cliSourcePath=QFileInfo(p).canonicalFilePath();break;}
    m_alreadyDeployed=QFile::exists(m_soDeployPath); m_bundledVersion=getDylibVersion(m_soSourcePath); m_deployedVersion=getDylibVersion(m_soDeployPath);
    m_updateAvailable=m_alreadyDeployed&&!m_bundledVersion.isEmpty()&&m_bundledVersion!=m_deployedVersion;
    if(!QDir(m_steamPath).exists())m_statusMessage="Steam.app was not found.";
    else if(m_soSourcePath.isEmpty())m_statusMessage="cloud_redirect.dylib is not bundled with this build.";
    else if(m_steamRunning)m_statusMessage="Steam is running. Quit Steam before using the CloudRedirect launcher.";
    else if(m_updateAvailable)m_statusMessage="A newer CloudRedirect dylib is available.";
    else if(m_alreadyDeployed)m_statusMessage="CloudRedirect is installed and ready.";
    else m_statusMessage="Ready to install CloudRedirect for Steam.";
    emit checkCompleted(); emit statusMessageChanged();
}
bool Deployer::deploy(){
    if(m_soSourcePath.isEmpty()){m_statusMessage="Cannot install: cloud_redirect.dylib is missing.";emit statusMessageChanged();emit deployCompleted(false);return false;}
    QDir().mkpath(crDataDir());
    if(QFile::exists(m_soDeployPath)&&!QFile::remove(m_soDeployPath)){m_statusMessage="Cannot replace the deployed dylib. Quit Steam first.";emit statusMessageChanged();emit deployCompleted(false);return false;}
    if(!QFile::copy(m_soSourcePath,m_soDeployPath)){m_statusMessage="Failed to copy cloud_redirect.dylib.";emit statusMessageChanged();emit deployCompleted(false);return false;}
    QFile::setPermissions(m_soDeployPath,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther);
    if(!m_cliSourcePath.isEmpty()){if(QFile::exists(m_cliDeployPath))QFile::remove(m_cliDeployPath);if(QFile::copy(m_cliSourcePath,m_cliDeployPath))QFile::setPermissions(m_cliDeployPath,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther);}
    m_alreadyDeployed=true;m_updateAvailable=false;m_deployedVersion=getDylibVersion(m_soDeployPath);m_statusMessage="CloudRedirect installed. Launch Steam with the injected dylib.";
    emit statusMessageChanged();emit checkCompleted();emit deployCompleted(true);return true;
}
bool Deployer::update(){return deploy();}
bool Deployer::undeploy(){if(m_steamRunning){m_statusMessage="Quit Steam before removing the dylib.";emit statusMessageChanged();return false;}QFile::remove(m_soDeployPath);QFile::remove(m_cliDeployPath);m_alreadyDeployed=false;m_deployedVersion.clear();m_statusMessage="CloudRedirect removed.";emit statusMessageChanged();emit checkCompleted();return true;}
bool Deployer::purgeAll(){if(m_steamRunning)return false;undeploy();QDir(crConfigDir()).removeRecursively();QDir(crDataDir()).removeRecursively();m_statusMessage="CloudRedirect data removed.";emit statusMessageChanged();emit checkCompleted();return true;}
bool Deployer::launchSteamWithCloudRedirect(){
    if(!m_alreadyDeployed&&!deploy())return false; m_steamRunning=detectSteamRunning();
    if(m_steamRunning){m_statusMessage="Steam is already running. Quit it completely first.";emit statusMessageChanged();emit checkCompleted();return false;}
    const QString exe=steamExecutable(); if(!QFile::exists(exe)){m_statusMessage="Steam executable not found at "+exe;emit statusMessageChanged();return false;}
    QString signatureReason;
    if (!steamAllowsDyldInjection(exe, signatureReason)) {
        m_statusMessage = "Steam cannot accept DYLD injection: " + signatureReason;
        emit statusMessageChanged();
        return false;
    }
    if(!QProcess::startDetached("/usr/bin/env",{"DYLD_INSERT_LIBRARIES="+m_soDeployPath,exe},QFileInfo(exe).absolutePath())){m_statusMessage="Failed to launch Steam with DYLD_INSERT_LIBRARIES.";emit statusMessageChanged();return false;}
    m_statusMessage="Steam launched with CloudRedirect.";emit statusMessageChanged();QTimer::singleShot(1500,this,&Deployer::checkPrerequisites);return true;
}
