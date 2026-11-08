#pragma once
#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>

class QTabWidget;
class QLabel;
class QStackedWidget;
class QToolButton;
class QJsonArray;

namespace cv {

class ChatView;
class EditorArea;
class FileTree;
class LoginDialog;

class MainWindow : public QMainWindow
{
public:
    MainWindow();

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void setupMenus();
    void onConnection(bool connected, const QString &detail);
    void onEvent(const QJsonObject &event);
    void syncSessions(const QJsonArray &sessions);
    ChatView *currentChat() const;
    void currentSessionChanged();
    void updateTabTitle(ChatView *chat);

    void newSession();
    void renameSession();
    void closeSession(ChatView *chat);

    void refreshAuth(bool promptIfLoggedOut = false);
    void openLogin();

    EditorArea *m_editors;
    FileTree *m_files;
    QStackedWidget *m_sessionStack;
    QTabWidget *m_sessionTabs;
    QLabel *m_connLabel;
    QLabel *m_authLabel;
    QToolButton *m_loginButton;
    QPointer<LoginDialog> m_login;

    QHash<QString, ChatView *> m_chats;
    QString m_host = QStringLiteral("localhost");
    QString m_home;
    QString m_selectOnCreate;
    bool m_authPromptShown = false;
};

} // namespace cv
