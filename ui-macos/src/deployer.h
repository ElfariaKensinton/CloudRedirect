#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class Deployer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool slssteamInstalled READ slssteamInstalled NOTIFY checkCompleted)
    Q_PROPERTY(bool headcrabInstalled READ headcrabInstalled NOTIFY checkCompleted)
    Q_PROPERTY(bool alreadyDeployed READ alreadyDeployed NOTIFY checkCompleted)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY checkCompleted)
    Q_PROPERTY(bool slsCloudBlocked READ slsCloudBlocked NOTIFY checkCompleted)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString bundledVersion READ bundledVersion NOTIFY checkCompleted)
    Q_PROPERTY(QString deployedVersion READ deployedVersion NOTIFY checkCompleted)
    Q_PROPERTY(QString steamPath READ steamPath NOTIFY checkCompleted)
    Q_PROPERTY(bool steamRunning READ steamRunning NOTIFY checkCompleted)
public:
    explicit Deployer(QObject *parent = nullptr);
    bool slssteamInstalled() const; bool headcrabInstalled() const; bool alreadyDeployed() const;
    bool updateAvailable() const; bool slsCloudBlocked() const; QString statusMessage() const;
    QString bundledVersion() const; QString deployedVersion() const; QString steamPath() const; bool steamRunning() const;
    Q_INVOKABLE void checkPrerequisites();
    Q_INVOKABLE bool deploy(); Q_INVOKABLE bool undeploy(); Q_INVOKABLE bool update(); Q_INVOKABLE bool purgeAll();
    Q_INVOKABLE bool launchSteamWithCloudRedirect();
signals:
    void checkCompleted(); void statusMessageChanged(); void deployCompleted(bool success);
private:
    bool detectSteamRunning() const;
    bool m_slssteamInstalled=false, m_headcrabInstalled=false, m_alreadyDeployed=false;
    bool m_updateAvailable=false, m_slsCloudBlocked=false, m_steamRunning=false;
    QString m_statusMessage, m_steamPath, m_soSourcePath, m_soDeployPath, m_cliSourcePath, m_cliDeployPath;
    QString m_bundledVersion, m_deployedVersion;
};
