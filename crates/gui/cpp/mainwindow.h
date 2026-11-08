#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QSet>

class QDockWidget;
class QLabel;
class QMenu;
class QStackedWidget;
class QToolButton;
class QJsonArray;

namespace cv {

class ChangesPanel;
class ChatView;
class Connection;
class DockStripes;
class EditorArea;
class FileTree;
class LoginDialog;
class SessionBar;
class TerminalPanel;

class MainWindow : public QMainWindow
{
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void setupMenus();
    void rebuildHostsMenu();

    Connection *addConnection(const QString &id, const QString &label, const QString &sshTarget);
    void loadHosts();
    void saveHosts();
    void addHost();
    void removeHost(Connection *conn);
    void onConnectionState(Connection *conn);
    void onEvent(Connection *conn, const QJsonObject &event);
    void updateConnectionLabel();

    void syncSessions(Connection *conn, const QJsonArray &sessions);
    ChatView *currentChat() const;
    Connection *currentConnection() const;
    void currentSessionChanged();
    void updateSessionEntry(ChatView *chat);
    void updateChatTitle();

    void newSession(Connection *preferred = nullptr);
    void renameSession(ChatView *chat);
    void closeSession(ChatView *chat);
    // Lists Claude's saved conversations for a folder and opens the chosen one as a session.
    void resumeConversation(Connection *conn, const QString &cwd);
    void requestCreateSession(Connection *conn, const QJsonObject &args, const QString &title);

    void refreshAuth(Connection *conn, bool promptIfLoggedOut = false);
    void updateAuthLabel();
    void openLogin(Connection *conn);

    EditorArea *m_editors;
    QStackedWidget *m_editorStack; // placeholder or the editor tabs
    FileTree *m_files;
    ChangesPanel *m_changes;
    QStackedWidget *m_sessionStack;
    QStackedWidget *m_chatStack;
    SessionBar *m_sessionBar;
    QDockWidget *m_chatDock;
    TerminalPanel *m_terminals;
    QDockWidget *m_terminalDock;
    DockStripes *m_stripes;
    QMenu *m_hostsMenu;
    QLabel *m_connLabel;
    QLabel *m_authLabel;
    QToolButton *m_loginButton;
    QPointer<LoginDialog> m_login;

    QList<Connection *> m_conns; // owned; the local connection is first
    QHash<QString, ChatView *> m_chats; // key: connection id + '/' + session id
    QHash<QString, QJsonObject> m_auth; // connection id -> auth status
    QSet<QString> m_authPrompted;       // connections we already offered a login for
    QSet<QString> m_newHosts;           // hosts added this run, awaiting first connect
    QString m_selectOnCreate;           // chat key to focus once it appears
};

} // namespace cv
