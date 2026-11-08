#include "codeview/cpp/app.h"
#include "codeview/src/main.rs.h"
#include "connection.h"
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
    QApplication::setOrganizationName(QStringLiteral("codeview")); // settings: ~/.config/codeview/codeview.conf
    QApplication::setApplicationName(QStringLiteral("codeview"));
    QApplication::setApplicationDisplayName(QStringLiteral("Code View"));
    QApplication::setDesktopFileName(QStringLiteral("codeview"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("applications-development")));

    g_alive = true; // before MainWindow opens connections
    auto *window = new MainWindow;
    window->show();
    const int rc = app.exec();
    g_alive = false;
    delete window;
    return rc;
}

static QString qstr(rust::Str s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

// Both called from Rust client threads.
void deliver_message(rust::Str conn, rust::Str json)
{
    if (!g_alive) return;
    QByteArray line(json.data(), static_cast<qsizetype>(json.size()));
    QMetaObject::invokeMethod(qApp, [id = qstr(conn), line] {
        if (Connection *c = Connection::find(id)) c->handleLine(line);
    }, Qt::QueuedConnection);
}

void deliver_connection_state(rust::Str conn, bool connected, rust::Str detail)
{
    if (!g_alive) return;
    QMetaObject::invokeMethod(qApp, [id = qstr(conn), connected, text = qstr(detail)] {
        if (Connection *c = Connection::find(id)) c->handleConnection(connected, text);
    }, Qt::QueuedConnection);
}

} // namespace cv
