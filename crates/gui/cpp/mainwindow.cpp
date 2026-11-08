#include "mainwindow.h"
#include "chatview.h"
#include "daemonclient.h"
#include "editorarea.h"
#include "filetree.h"
#include "logindialog.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolButton>

namespace cv {

MainWindow::MainWindow()
{
    resize(1500, 950);
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks);

    m_editors = new EditorArea;
    auto *editorPlaceholder = new QLabel(QStringLiteral("Open a file from the Files panel, or click a file link in the chat."));
    editorPlaceholder->setAlignment(Qt::AlignCenter);
    editorPlaceholder->setEnabled(false);
    auto *editorStack = new QStackedWidget;
    editorStack->addWidget(editorPlaceholder);
    editorStack->addWidget(m_editors);
    auto syncEditorStack = [this, editorStack] { editorStack->setCurrentIndex(m_editors->count() ? 1 : 0); };
    connect(m_editors, &QTabWidget::currentChanged, this, syncEditorStack);
    setCentralWidget(editorStack);

    // Files (left).
    m_files = new FileTree;
    m_files->onOpenFile = [this](const QString &p) { m_editors->openFile(p); };
    auto *filesDock = new QDockWidget(QStringLiteral("Files"));
    filesDock->setObjectName(QStringLiteral("files"));
    filesDock->setWidget(m_files);
    addDockWidget(Qt::LeftDockWidgetArea, filesDock);

    // Sessions (right).
    m_sessionTabs = new QTabWidget;
    m_sessionTabs->setTabsClosable(true);
    m_sessionTabs->setMovable(true);
    m_sessionTabs->setDocumentMode(true);
    auto *addButton = new QToolButton;
    addButton->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    addButton->setToolTip(QStringLiteral("New session"));
    addButton->setAutoRaise(true);
    connect(addButton, &QToolButton::clicked, this, [this] { newSession(); });
    m_sessionTabs->setCornerWidget(addButton, Qt::TopRightCorner);
    connect(m_sessionTabs, &QTabWidget::currentChanged, this, [this] { currentSessionChanged(); });
    connect(m_sessionTabs, &QTabWidget::tabCloseRequested, this,
            [this](int i) { closeSession(static_cast<ChatView *>(m_sessionTabs->widget(i))); });

    auto *empty = new QWidget;
    auto *emptyLayout = new QVBoxLayout(empty);
    auto *emptyLabel = new QLabel(QStringLiteral("No sessions yet."));
    emptyLabel->setAlignment(Qt::AlignCenter);
    auto *emptyButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("New Session…"));
    connect(emptyButton, &QPushButton::clicked, this, [this] { newSession(); });
    emptyLayout->addStretch();
    emptyLayout->addWidget(emptyLabel);
    emptyLayout->addWidget(emptyButton, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();

    m_sessionStack = new QStackedWidget;
    m_sessionStack->addWidget(empty);
    m_sessionStack->addWidget(m_sessionTabs);
    auto *chatDock = new QDockWidget(QStringLiteral("Claude"));
    chatDock->setObjectName(QStringLiteral("claude"));
    chatDock->setWidget(m_sessionStack);
    addDockWidget(Qt::RightDockWidgetArea, chatDock);
    resizeDocks({filesDock, chatDock}, {280, 620}, Qt::Horizontal);

    // Status bar.
    m_connLabel = new QLabel(QStringLiteral("Connecting to codeviewd…"));
    m_authLabel = new QLabel;
    m_loginButton = new QToolButton;
    m_loginButton->setText(QStringLiteral("Log in…"));
    m_loginButton->setAutoRaise(true);
    m_loginButton->hide();
    connect(m_loginButton, &QToolButton::clicked, this, [this] { openLogin(); });
    statusBar()->addWidget(m_connLabel, 1);
    statusBar()->addPermanentWidget(m_authLabel);
    statusBar()->addPermanentWidget(m_loginButton);

    setupMenus();

    auto &d = DaemonClient::instance();
    d.onConnectionChanged = [this](bool c, const QString &detail) { onConnection(c, detail); };
    d.onEvent = [this](const QJsonObject &e) { onEvent(e); };
}

void MainWindow::setupMenus()
{
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    // No shortcut here: the editor view's own Ctrl+S would make it ambiguous.
    file->addAction(QIcon::fromTheme(QStringLiteral("document-save")), QStringLiteral("Save (Ctrl+S in editor)"), this,
                    [this] { m_editors->saveCurrent(); });
    file->addAction(QIcon::fromTheme(QStringLiteral("tab-close")), QStringLiteral("Close File"), this,
                    [this] { m_editors->closeTab(m_editors->currentIndex()); });
    file->addSeparator();
    file->addAction(QIcon::fromTheme(QStringLiteral("application-exit")), QStringLiteral("&Quit"), QKeySequence::Quit, this,
                    &QWidget::close);

    QMenu *session = menuBar()->addMenu(QStringLiteral("&Session"));
    session->addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("&New Session…"),
                       QKeySequence(QStringLiteral("Ctrl+Shift+N")), this, [this] { newSession(); });
    session->addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), QStringLiteral("&Rename Session…"), this,
                       [this] { renameSession(); });
    session->addAction(QIcon::fromTheme(QStringLiteral("tab-close")), QStringLiteral("&Close Session…"), this,
                       [this] { closeSession(currentChat()); });
    session->addSeparator();
    session->addAction(QStringLiteral("Focus Prompt"), QKeySequence(QStringLiteral("Ctrl+L")), this, [this] {
        if (auto *c = currentChat()) c->focusPrompt();
    });
    // Alt+1..9 jumps to a session.
    for (int i = 0; i < 9; ++i) {
        auto *a = new QAction(this);
        a->setShortcut(QKeySequence(QStringLiteral("Alt+%1").arg(i + 1)));
        connect(a, &QAction::triggered, this, [this, i] {
            if (i < m_sessionTabs->count()) {
                m_sessionTabs->setCurrentIndex(i);
                currentChat()->focusPrompt();
            }
        });
        addAction(a);
    }

    QMenu *claude = menuBar()->addMenu(QStringLiteral("&Claude"));
    claude->addAction(QIcon::fromTheme(QStringLiteral("dialog-password")), QStringLiteral("&Log In…"), this,
                      [this] { openLogin(); });
    claude->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Check Login Status"), this,
                      [this] { refreshAuth(); });
}

// ---- daemon connection ----

void MainWindow::onConnection(bool connected, const QString &detail)
{
    if (!connected) {
        m_connLabel->setText(detail.isEmpty() ? QStringLiteral("Disconnected — retrying…")
                                              : QStringLiteral("%1 — retrying…").arg(detail));
        return;
    }
    m_connLabel->setText(QStringLiteral("Connected"));
    auto &d = DaemonClient::instance();
    d.request(QStringLiteral("hello"), {}, [this](bool ok, const QJsonValue &data, const QString &) {
        if (!ok) return;
        m_host = data[QStringLiteral("hostname")].toString();
        m_home = data[QStringLiteral("home")].toString();
        m_connLabel->setText(QStringLiteral("Connected to %1").arg(m_host));
        setWindowTitle(m_host); // Qt appends the app name
    });
    // Reload every transcript: events may have been missed while disconnected.
    for (ChatView *c : std::as_const(m_chats)) c->loadHistory();
    d.request(QStringLiteral("list_sessions"), {}, [this](bool ok, const QJsonValue &data, const QString &) {
        if (ok) syncSessions(data.toArray());
    });
    refreshAuth(true);
}

void MainWindow::onEvent(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("sessions_changed")) {
        syncSessions(event.value(QStringLiteral("sessions")).toArray());
    } else if (type == QLatin1String("session")) {
        if (ChatView *c = m_chats.value(event.value(QStringLiteral("session")).toString()))
            c->handleEvent(event.value(QStringLiteral("seq")).toInteger(), event.value(QStringLiteral("event")).toObject());
    } else if (type == QLatin1String("login_output")) {
        if (m_login) m_login->appendOutput(event.value(QStringLiteral("text")).toString());
    } else if (type == QLatin1String("login_finished")) {
        if (m_login) m_login->finished(event.value(QStringLiteral("success")).toBool());
        refreshAuth();
    }
}

// ---- sessions ----

void MainWindow::syncSessions(const QJsonArray &sessions)
{
    QSet<QString> seen;
    for (const QJsonValue &v : sessions) {
        const QJsonObject info = v.toObject();
        const QString id = info.value(QStringLiteral("id")).toString();
        seen.insert(id);
        ChatView *chat = m_chats.value(id);
        if (!chat) {
            chat = new ChatView(info);
            chat->onOpenFile = [this](const QString &p) { m_editors->openFile(p); };
            chat->onFileChanged = [this](const QString &p) { m_editors->reloadIfUnmodified(p); };
            chat->onAuthProblem = [this] { refreshAuth(true); };
            m_chats.insert(id, chat);
            m_sessionTabs->addTab(chat, info.value(QStringLiteral("name")).toString());
            chat->loadHistory();
        } else {
            chat->setInfo(info);
        }
        updateTabTitle(chat);
        if (id == m_selectOnCreate) {
            m_selectOnCreate.clear();
            m_sessionTabs->setCurrentWidget(chat);
            chat->focusPrompt();
        }
    }
    for (auto it = m_chats.begin(); it != m_chats.end();) {
        if (!seen.contains(it.key())) {
            delete it.value(); // removes its tab
            it = m_chats.erase(it);
        } else {
            ++it;
        }
    }
    m_sessionStack->setCurrentIndex(m_chats.isEmpty() ? 0 : 1);
    currentSessionChanged();
}

void MainWindow::updateTabTitle(ChatView *chat)
{
    const int i = m_sessionTabs->indexOf(chat);
    if (i < 0) return;
    const QString status = chat->status();
    QString icon;
    if (status == QLatin1String("busy")) icon = QStringLiteral("media-playback-start");
    else if (status == QLatin1String("waiting_permission")) icon = QStringLiteral("dialog-warning");
    m_sessionTabs->setTabIcon(i, icon.isEmpty() ? QIcon() : QIcon::fromTheme(icon));
    m_sessionTabs->setTabText(i, chat->name());
    m_sessionTabs->setTabToolTip(i, chat->cwd());
}

ChatView *MainWindow::currentChat() const { return static_cast<ChatView *>(m_sessionTabs->currentWidget()); }

void MainWindow::currentSessionChanged()
{
    if (ChatView *c = currentChat()) m_files->setRoot(c->cwd());
}

void MainWindow::newSession()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("New Session"));
    auto *name = new QLineEdit;
    name->setPlaceholderText(QStringLiteral("Defaults to the folder name"));
    auto *dir = new QLineEdit(currentChat() ? currentChat()->cwd() : (m_home.isEmpty() ? QDir::homePath() : m_home));
    auto *browse = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")), QString());
    connect(browse, &QPushButton::clicked, &dlg, [&dlg, dir] {
        const QString p = QFileDialog::getExistingDirectory(&dlg, QStringLiteral("Project folder"), dir->text());
        if (!p.isEmpty()) dir->setText(p);
    });
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(dir, 1);
    dirRow->addWidget(browse);
    auto *mode = new QComboBox;
    mode->addItem(QStringLiteral("Use my Claude settings"), QString());
    mode->addItem(QStringLiteral("Ask before every change"), QStringLiteral("manual"));
    mode->addItem(QStringLiteral("Accept file edits automatically"), QStringLiteral("acceptEdits"));
    mode->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
    mode->addItem(QStringLiteral("Plan only (no changes)"), QStringLiteral("plan"));
    mode->addItem(QStringLiteral("Bypass all permission checks"), QStringLiteral("bypassPermissions"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *form = new QFormLayout(&dlg);
    form->addRow(QStringLiteral("Name:"), name);
    form->addRow(QStringLiteral("Folder:"), dirRow);
    form->addRow(QStringLiteral("Permissions:"), mode);
    form->addRow(buttons);
    dlg.resize(560, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted) return;

    QJsonObject args{{QStringLiteral("name"), name->text().trimmed()}, {QStringLiteral("cwd"), dir->text().trimmed()}};
    if (!mode->currentData().toString().isEmpty()) args.insert(QStringLiteral("permission_mode"), mode->currentData().toString());
    DaemonClient::instance().request(QStringLiteral("create_session"), args, [this](bool ok, const QJsonValue &data, const QString &error) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("New Session"), error);
            return;
        }
        const QString id = data[QStringLiteral("id")].toString();
        if (ChatView *c = m_chats.value(id)) {
            m_sessionTabs->setCurrentWidget(c);
            c->focusPrompt();
        } else {
            m_selectOnCreate = id; // the sessions_changed event hasn't arrived yet
        }
    });
}

void MainWindow::renameSession()
{
    ChatView *c = currentChat();
    if (!c) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Session"), QStringLiteral("Name:"),
                                               QLineEdit::Normal, c->name(), &ok);
    if (ok && !name.trimmed().isEmpty())
        DaemonClient::instance().request(QStringLiteral("rename_session"),
                                         {{QStringLiteral("session"), c->id()}, {QStringLiteral("name"), name.trimmed()}});
}

void MainWindow::closeSession(ChatView *c)
{
    if (!c) return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Close Session"),
        QStringLiteral("Close “%1”? Claude stops, but the conversation stays in Claude's history (claude --resume).").arg(c->name()));
    if (answer == QMessageBox::Yes)
        DaemonClient::instance().request(QStringLiteral("close_session"), {{QStringLiteral("session"), c->id()}});
}

// ---- auth ----

void MainWindow::refreshAuth(bool promptIfLoggedOut)
{
    DaemonClient::instance().request(QStringLiteral("auth_status"), {}, [this, promptIfLoggedOut](bool ok, const QJsonValue &data, const QString &error) {
        if (!ok) {
            m_authLabel->setText(QStringLiteral("Claude: unknown (%1)").arg(error));
            return;
        }
        const bool loggedIn = data[QStringLiteral("loggedIn")].toBool();
        m_loginButton->setVisible(!loggedIn);
        if (loggedIn) {
            const QString who = data[QStringLiteral("email")].toString();
            m_authLabel->setText(QStringLiteral("Claude: %1").arg(who.isEmpty() ? QStringLiteral("logged in") : who));
            m_authPromptShown = false;
            return;
        }
        m_authLabel->setText(QStringLiteral("Claude: not logged in"));
        if (promptIfLoggedOut && !m_authPromptShown && !m_login) {
            m_authPromptShown = true;
            if (QMessageBox::question(this, QStringLiteral("Claude"),
                                      QStringLiteral("Claude isn't logged in on %1. Log in now?").arg(m_host)) == QMessageBox::Yes)
                openLogin();
        }
    });
}

void MainWindow::openLogin()
{
    if (!m_login) {
        m_login = new LoginDialog(m_host, this);
        m_login->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_login->show();
    m_login->raise();
    m_login->start();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    // Sessions keep running in the daemon; only unsaved editor changes need asking about.
    if (m_editors->queryCloseAll()) e->accept();
    else e->ignore();
}

} // namespace cv
