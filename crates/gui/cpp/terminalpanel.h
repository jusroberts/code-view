#pragma once
#include <QHash>
#include <QWidget>

class QStackedWidget;
class QTabWidget;

namespace cv {

class Connection;

// Terminal tabs (Konsole's terminal component), one set per session.
// Remote sessions get `ssh -t host` terminals that start in the session's folder.
class TerminalPanel : public QWidget
{
public:
    explicit TerminalPanel(QWidget *parent = nullptr);

    // Switches to the given session's terminals. An empty key shows nothing.
    void setSession(const QString &key, Connection *conn, const QString &cwd);
    void removeSession(const QString &key);
    // `typed` is entered at the prompt without pressing Enter, so the user can review it.
    void newTerminal(const QString &typed = QString());
    // Opens a first terminal if the current session has none.
    void ensureTerminal();

private:
    QTabWidget *tabsFor(const QString &key);

    QStackedWidget *m_stack;
    QWidget *m_empty;
    QHash<QString, QTabWidget *> m_tabs;
    QString m_key;
    Connection *m_conn = nullptr;
    QString m_cwd;
};

} // namespace cv
