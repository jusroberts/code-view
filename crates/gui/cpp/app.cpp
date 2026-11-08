#include "codeview/cpp/app.h"
#include "codeview/src/main.rs.h"
#include "daemonclient.h"
#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <atomic>

namespace cv {

static std::atomic<bool> g_alive{false};

int run_app()
{
    static int argc = 1;
    static char arg0[] = "codeview";
    static char *argv[] = {arg0, nullptr};
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("codeview"));
    QApplication::setApplicationDisplayName(QStringLiteral("Code View"));
    QApplication::setDesktopFileName(QStringLiteral("codeview"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("applications-development")));

    auto *window = new MainWindow;
    window->show();
    g_alive = true;
    client_start();
    const int rc = app.exec();
    g_alive = false;
    delete window;
    return rc;
}

// Both called from the Rust client thread.
void deliver_message(rust::Str json)
{
    if (!g_alive) return;
    QByteArray line(json.data(), static_cast<qsizetype>(json.size()));
    QMetaObject::invokeMethod(qApp, [line] { DaemonClient::instance().handleLine(line); }, Qt::QueuedConnection);
}

void deliver_connection_state(bool connected, rust::Str detail)
{
    if (!g_alive) return;
    QString text = QString::fromUtf8(detail.data(), static_cast<qsizetype>(detail.size()));
    QMetaObject::invokeMethod(qApp, [connected, text] { DaemonClient::instance().handleConnection(connected, text); },
                              Qt::QueuedConnection);
}

} // namespace cv
