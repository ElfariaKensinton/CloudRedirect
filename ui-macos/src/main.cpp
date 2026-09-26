#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDebug>
#include <cstdlib>
#include "backend.h"
#include "deployer.h"
#include "oauthservice.h"

using namespace Qt::StringLiterals;

#ifndef CR_VERSION
#define CR_VERSION "2.6.5"
#endif

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName("CloudRedirect");
    app.setApplicationName("CloudRedirect");
    app.setApplicationVersion(CR_VERSION);
    
    QQmlApplicationEngine engine;

    // Register backend singletons
    Backend backend;
    Deployer deployer;
    OAuthService oauthService;

    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("deployer", &deployer);
    engine.rootContext()->setContextProperty("oauth", &oauthService);

    // Wire OAuth signals to backend
    QObject::connect(&oauthService, &OAuthService::authSucceeded, &backend, &Backend::refreshStatus);
    QObject::connect(&oauthService, &OAuthService::authSucceeded, &backend, &Backend::saveConfig);
    
    // Wire deployer signals to backend so Dashboard updates after deploy/undeploy
    QObject::connect(&deployer, &Deployer::checkCompleted, &backend, &Backend::refreshStatus);

    // Load QML from the compiled-in resource
    const QUrl url(u"qrc:/CloudRedirect/qml/Main.qml"_s);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { qCritical("QML load failed"); exit(-1); },
        Qt::QueuedConnection);
    engine.load(url);

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
