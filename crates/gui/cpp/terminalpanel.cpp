#include "terminalpanel.h"
#include "connection.h"

#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <kde_terminal_interface.h>

#include <QFileInfo>
#include <QLabel>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace cv {

namespace {
QString shellQuote(const QString &s)
{
    return QLatin1Char('\'') + QString(s).replace(QLatin1Char('\''), QLatin1String("'\\''")) + QLatin1Char('\'');
}
} // namespace

TerminalPanel::TerminalPanel(QWidget *parent)
    : QWidget(parent)
{
    m_empty = new QLabel(QStringLiteral("Select a session to open a terminal."));
    static_cast<QLabel *>(m_empty)->setAlignment(Qt::AlignCenter);
    m_empty->setEnabled(false);
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_empty);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);
}

QTabWidget *TerminalPanel::tabsFor(const QString &key)
{
    if (QTabWidget *tabs = m_tabs.value(key)) return tabs;
    auto *tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    auto *add = new QToolButton;
    add->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    add->setToolTip(QStringLiteral("New terminal (Ctrl+Shift+`)"));
    add->setAutoRaise(true);
    connect(add, &QToolButton::clicked, this, [this] { newTerminal(); });
    tabs->setCornerWidget(add, Qt::TopRightCorner);
    connect(tabs, &QTabWidget::tabCloseRequested, this, [tabs](int i) { delete tabs->widget(i); });
    m_tabs.insert(key, tabs);
    m_stack->addWidget(tabs);
    return tabs;
}

void TerminalPanel::setSession(const QString &key, Connection *conn, const QString &cwd)
{
    m_key = key;
    m_conn = conn;
    m_cwd = cwd;
    m_stack->setCurrentWidget(key.isEmpty() ? m_empty : tabsFor(key));
    if (isVisible()) ensureTerminal();
}

void TerminalPanel::removeSession(const QString &key)
{
    delete m_tabs.take(key); // deletes its terminals
    if (key == m_key) setSession(QString(), nullptr, QString());
}

void TerminalPanel::ensureTerminal()
{
    if (QTabWidget *tabs = m_tabs.value(m_key); tabs && tabs->count() == 0) newTerminal();
}

void TerminalPanel::newTerminal()
{
    if (m_key.isEmpty() || !m_conn) return;
    QTabWidget *tabs = tabsFor(m_key);
    const auto result = KPluginFactory::instantiatePlugin<KParts::ReadOnlyPart>(KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")), tabs);
    if (!result.plugin) {
        tabs->addTab(new QLabel(QStringLiteral("Couldn't load Konsole's terminal component (install konsole): ") + result.errorText),
                     QStringLiteral("Error"));
        return;
    }
    KParts::ReadOnlyPart *part = result.plugin;
    auto *term = qobject_cast<TerminalInterface *>(part);
    QWidget *w = part->widget();
    const QString title = m_conn->isLocal() ? QFileInfo(m_cwd).fileName() : m_conn->label();
    const int index = tabs->addTab(w, QIcon::fromTheme(QStringLiteral("utilities-terminal")), title);
    tabs->setCurrentIndex(index);
    // The part deletes itself when the shell exits; closing the tab deletes the widget, so delete the part too.
    connect(w, &QObject::destroyed, part, [part] { part->deleteLater(); });
    connect(part, &KParts::Part::setWindowCaption, w, [tabs, w](const QString &caption) {
        if (const int i = tabs->indexOf(w); i >= 0 && !caption.isEmpty()) tabs->setTabText(i, caption);
    });
    if (term) {
        if (m_conn->isLocal()) {
            term->showShellInDir(m_cwd);
        } else {
            const QString ssh = qEnvironmentVariable("CODEVIEW_SSH", QStringLiteral("ssh"));
            term->startProgram(ssh, {ssh, QStringLiteral("-t"), m_conn->sshTarget(),
                                     QStringLiteral("cd %1 && exec \"$SHELL\" -l").arg(shellQuote(m_cwd))});
        }
    }
    w->setFocus();
}

} // namespace cv
