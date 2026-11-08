#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QSet>

class QTabWidget;
class QLabel;
class QMenu;
class QStackedWidget;
class QToolButton;
class QJsonArray;

namespace cv {

class ChatView;
class Connection;
class EditorArea;
class FileTree;
class LoginDialog;

class MainWindow : public QMainWindow
{
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *e) override;

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
    void updateTabTitle(ChatView *chat);

    void newSession(Connection *preferred = nullptr);
    void renameSession();
    void closeSession(ChatView *chat);

    void refreshAuth(Connection *conn, bool promptIfLoggedOut = false);
    void updateAuthLabel();
    void openLogin(Connection *conn);

    EditorArea *m_editors;
    FileTree *m_files;
    QStackedWidget *m_sessionStack;
    QTabWidget *m_sessionTabs;
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
