#include "mainwindow.h"
#include "changespanel.h"
#include "chatview.h"
#include "connection.h"
#include "editorarea.h"
#include "filetree.h"
#include "logindialog.h"
#include "sessionbar.h"
#include "terminalpanel.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

namespace cv {

namespace {

QString chatKey(Connection *conn, const QString &sessionId) { return conn->id() + QLatin1Char('/') + sessionId; }

// Host aliases from ~/.ssh/config, for the Add Host dialog.
QStringList sshConfigHosts()
{
    QStringList hosts;
    QFile f(QDir::homePath() + QStringLiteral("/.ssh/config"));
    if (!f.open(QIODevice::ReadOnly)) return hosts;
    static const QRegularExpression hostLine(QStringLiteral(R"(^\s*Host\s+(.+)$)"), QRegularExpression::CaseInsensitiveOption);
    QTextStream in(&f);
    while (!in.atEnd()) {
        const auto m = hostLine.match(in.readLine());
        if (!m.hasMatch()) continue;
        for (const QString &h : m.captured(1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
            if (!h.contains(QLatin1Char('*')) && !h.contains(QLatin1Char('?')) && !h.startsWith(QLatin1Char('!'))) hosts << h;
    }
    return hosts;
}

// A plain-language hint for common ssh/bridge failures.
QString connectionHint(const QString &detail)
{
    if (detail.contains(QLatin1String("command not found")) || detail.contains(QLatin1String("No such file")))
        return QStringLiteral("codeviewd isn't installed on that host (or isn't on its PATH). Install it there, e.g. "
                              "<tt>cargo install --path crates/daemon</tt> from this repo.");
    if (detail.contains(QLatin1String("Host key verification failed")))
        return QStringLiteral("SSH doesn't know this host's key yet. Run <tt>ssh HOST</tt> once in a terminal to accept it.");
    if (detail.contains(QLatin1String("Permission denied")))
        return QStringLiteral("SSH needs passwordless key login to this host (Code View can't answer password prompts).");
    return {};
}

// Folder picker that browses through a host's daemon (local or remote).
QString pickFolder(QWidget *parent, Connection *conn, const QString &start)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Choose a folder on %1").arg(conn->label()));
    auto *path = new QLineEdit(start);
    auto *tree = new FileTree(true);
    tree->setShowHidden(false);
    tree->onSelect = [path](const QString &p) { path->setText(p); };
    // Show the whole tree from / with the starting folder selected, so you can still go up.
    tree->reveal(start.isEmpty() ? conn->home() : start);
    tree->setRoot(conn, QStringLiteral("/"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(tree, 1);
    layout->addWidget(path);
    layout->addWidget(buttons);
    dlg.resize(520, 560);
    return dlg.exec() == QDialog::Accepted ? path->text().trimmed() : QString();
}

} // namespace

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
    connect(m_editors, &QTabWidget::currentChanged, this, [this, editorStack] { editorStack->setCurrentIndex(m_editors->count() ? 1 : 0); });
    setCentralWidget(editorStack);

    // Files (left).
    m_files = new FileTree;
    m_files->onOpenFile = [this](const QString &p) { m_editors->openFile(m_files->connection(), p); };
    auto *filesDock = new QDockWidget(QStringLiteral("Files"));
    filesDock->setObjectName(QStringLiteral("files"));
    filesDock->setWidget(m_files);
    addDockWidget(Qt::LeftDockWidgetArea, filesDock);

    // Changes (tabbed with Files): Claude's edits, uncommitted work and the branch diff.
    m_changes = new ChangesPanel;
    m_changes->onOpenFile = [this](Connection *conn, const QString &p) { m_editors->openFile(conn, p); };
    m_changes->onReverted = [this](Connection *conn, const QString &p) { m_editors->reloadIfUnmodified(conn, p); };
    m_changes->onShowDiff = [this](Connection *conn, const QString &path, const QString &base, const QString &title) {
        ChatView *c = currentChat();
        if (!conn || !c) return;
        QJsonObject args{{QStringLiteral("cwd"), c->cwd()}, {QStringLiteral("path"), path}};
        if (!base.isEmpty()) args.insert(QStringLiteral("base"), base);
        conn->request(QStringLiteral("git_diff"), args, [this, key = conn->id() + QLatin1Char(':') + path + QLatin1Char('@') + base,
                                                         title, tip = conn->isLocal() ? path : conn->label() + QLatin1Char(':') + path](
                                                            bool ok, const QJsonValue &data, const QString &error) {
            if (!ok) QMessageBox::warning(this, QStringLiteral("Diff"), error);
            else m_editors->showDiff(key, title, tip, data[QStringLiteral("diff")].toString());
        });
    };
    auto *changesDock = new QDockWidget(QStringLiteral("Changes"));
    changesDock->setObjectName(QStringLiteral("changes"));
    changesDock->setWidget(m_changes);
    m_editors->onSaved = [this] { m_changes->scheduleRefresh(); };

    // Session list (far left); the selected session drives the rest of the window.
    m_sessionBar = new SessionBar;
    m_sessionBar->onNew = [this] { newSession(); };
    m_sessionBar->onRename = [this](const QString &key) { renameSession(m_chats.value(key)); };
    m_sessionBar->onClose = [this](const QString &key) { closeSession(m_chats.value(key)); };
    m_sessionBar->onCurrentChanged = [this](const QString &key) {
        if (ChatView *c = m_chats.value(key)) m_chatStack->setCurrentWidget(c);
        currentSessionChanged();
    };
    auto *sessionsDock = new QDockWidget(QStringLiteral("Sessions"));
    sessionsDock->setObjectName(QStringLiteral("sessions"));
    sessionsDock->setTitleBarWidget(new QWidget); // the bar has its own buttons
    sessionsDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    sessionsDock->setWidget(m_sessionBar);
    addDockWidget(Qt::LeftDockWidgetArea, sessionsDock);
    splitDockWidget(sessionsDock, filesDock, Qt::Horizontal);
    tabifyDockWidget(filesDock, changesDock);
    filesDock->raise();

    // Claude chat for the selected session (right).
    m_chatStack = new QStackedWidget;

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
    m_sessionStack->addWidget(m_chatStack);
    auto *chatDock = m_chatDock = new QDockWidget(QStringLiteral("Claude"));
    chatDock->setObjectName(QStringLiteral("claude"));
    chatDock->setWidget(m_sessionStack);
    addDockWidget(Qt::RightDockWidgetArea, chatDock);
    resizeDocks({filesDock, chatDock}, {280, 620}, Qt::Horizontal);

    // Terminals (bottom), hidden until asked for.
    m_terminals = new TerminalPanel;
    m_terminalDock = new QDockWidget(QStringLiteral("Terminal"));
    m_terminalDock->setObjectName(QStringLiteral("terminal"));
    m_terminalDock->setWidget(m_terminals);
    addDockWidget(Qt::BottomDockWidgetArea, m_terminalDock);
    resizeDocks({m_terminalDock}, {260}, Qt::Vertical);
    m_terminalDock->hide();
    connect(m_terminalDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) m_terminals->ensureTerminal();
    });

    // Status bar.
    m_connLabel = new QLabel;
    m_connLabel->setTextFormat(Qt::RichText);
    m_authLabel = new QLabel;
    m_loginButton = new QToolButton;
    m_loginButton->setText(QStringLiteral("Log in…"));
    m_loginButton->setAutoRaise(true);
    m_loginButton->hide();
    connect(m_loginButton, &QToolButton::clicked, this, [this] { openLogin(currentConnection()); });
    statusBar()->addWidget(m_connLabel, 1);
    statusBar()->addPermanentWidget(m_authLabel);
    statusBar()->addPermanentWidget(m_loginButton);

    setupMenus();

    addConnection(QString::fromLatin1(Connection::kLocalId), QStringLiteral("local"), QString());
    loadHosts();
    updateConnectionLabel();
}

MainWindow::~MainWindow()
{
    // Chats and editors reference connections; drop them first.
    delete m_chatStack;
    m_chats.clear();
    qDeleteAll(m_conns);
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
                       [this] { renameSession(currentChat()); });
    session->addAction(QIcon::fromTheme(QStringLiteral("tab-close")), QStringLiteral("&Close Session…"), this,
                       [this] { closeSession(currentChat()); });
    session->addAction(QIcon::fromTheme(QStringLiteral("sidebar-collapse-left")), QStringLiteral("Collapse Session List"),
                       QKeySequence(QStringLiteral("Ctrl+Shift+B")), this,
                       [this] { m_sessionBar->setCollapsed(!m_sessionBar->isCollapsed()); });
    session->addSeparator();
    session->addAction(QStringLiteral("Focus Prompt"), QKeySequence(QStringLiteral("Ctrl+L")), this, [this] {
        if (auto *c = currentChat()) c->focusPrompt();
    });
    // Alt+1..9 jumps to a session.
    for (int i = 0; i < 9; ++i) {
        auto *a = new QAction(this);
        a->setShortcut(QKeySequence(QStringLiteral("Alt+%1").arg(i + 1)));
        connect(a, &QAction::triggered, this, [this, i] {
            if (ChatView *c = m_chats.value(m_sessionBar->keyAt(i))) {
                m_sessionBar->setCurrentKey(m_sessionBar->keyAt(i));
                c->focusPrompt();
            }
        });
        addAction(a);
    }

    QMenu *terminal = menuBar()->addMenu(QStringLiteral("&Terminal"));
    terminal->addAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")), QStringLiteral("Show/Hide &Terminal"),
                        QKeySequence(QStringLiteral("Ctrl+`")), this, [this] {
                            m_terminalDock->setVisible(!m_terminalDock->isVisible());
                            if (m_terminalDock->isVisible()) m_terminals->setFocus();
                        });
    terminal->addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("&New Terminal"),
                        QKeySequence(QStringLiteral("Ctrl+Shift+`")), this, [this] {
                            m_terminals->newTerminal(); // before show(), so showing doesn't add a second one
                            m_terminalDock->show();
                        });

    m_hostsMenu = menuBar()->addMenu(QStringLiteral("&Hosts"));
    connect(m_hostsMenu, &QMenu::aboutToShow, this, [this] { rebuildHostsMenu(); });

    QMenu *claude = menuBar()->addMenu(QStringLiteral("&Claude"));
    claude->addAction(QIcon::fromTheme(QStringLiteral("dialog-password")), QStringLiteral("&Log In on Current Host…"), this,
                      [this] { openLogin(currentConnection()); });
    claude->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Check Login Status"), this,
                      [this] { refreshAuth(currentConnection()); });
}

void MainWindow::rebuildHostsMenu()
{
    m_hostsMenu->clear();
    m_hostsMenu->addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("&Add Remote Host…"), this,
                           [this] { addHost(); });
    m_hostsMenu->addSeparator();
    for (Connection *conn : std::as_const(m_conns)) {
        const QString state = conn->connected() ? QStringLiteral("connected") : QStringLiteral("not connected");
        QMenu *sub = m_hostsMenu->addMenu(
            QIcon::fromTheme(conn->connected() ? QStringLiteral("network-connect") : QStringLiteral("network-disconnect")),
            QStringLiteral("%1 (%2)").arg(conn->label(), state));
        if (!conn->connected() && !conn->detail().isEmpty()) {
            auto *why = sub->addAction(conn->detail().left(120));
            why->setEnabled(false);
            sub->addSeparator();
        }
        sub->addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("New Session…"), this,
                       [this, conn] { newSession(conn); })->setEnabled(conn->connected());
        sub->addAction(QIcon::fromTheme(QStringLiteral("dialog-password")), QStringLiteral("Log In to Claude…"), this,
                       [this, conn] { openLogin(conn); })->setEnabled(conn->connected());
        sub->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Reconnect"), this, [conn] { conn->open(); });
        if (!conn->isLocal()) {
            sub->addSeparator();
            sub->addAction(QIcon::fromTheme(QStringLiteral("list-remove")), QStringLiteral("Remove Host"), this,
                           [this, conn] { removeHost(conn); });
        }
    }
}

// ---- hosts & connections ----

Connection *MainWindow::addConnection(const QString &id, const QString &label, const QString &sshTarget)
{
    auto *conn = new Connection(id, label, sshTarget);
    conn->onStateChanged = [this, conn] { onConnectionState(conn); };
    conn->onEvent = [this, conn](const QJsonObject &e) { onEvent(conn, e); };
    m_conns.append(conn);
    conn->open();
    return conn;
}

void MainWindow::loadHosts()
{
    QSettings settings;
    const int n = settings.beginReadArray(QStringLiteral("hosts"));
    for (int i = 0; i < n; ++i) {
        settings.setArrayIndex(i);
        const QString target = settings.value(QStringLiteral("target")).toString();
        if (target.isEmpty()) continue;
        addConnection(QStringLiteral("ssh:") + target, settings.value(QStringLiteral("label"), target).toString(), target);
    }
    settings.endArray();
}

void MainWindow::saveHosts()
{
    QSettings settings;
    settings.beginWriteArray(QStringLiteral("hosts"));
    int i = 0;
    for (Connection *conn : std::as_const(m_conns)) {
        if (conn->isLocal()) continue;
        settings.setArrayIndex(i++);
        settings.setValue(QStringLiteral("label"), conn->label());
        settings.setValue(QStringLiteral("target"), conn->sshTarget());
    }
    settings.endArray();
}

void MainWindow::addHost()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Add Remote Host"));
    auto *target = new QComboBox;
    target->setEditable(true);
    target->addItems(sshConfigHosts());
    target->lineEdit()->setPlaceholderText(QStringLiteral("user@host or an alias from ~/.ssh/config"));
    auto *label = new QLineEdit;
    label->setPlaceholderText(QStringLiteral("Defaults to the SSH target"));
    auto *note = new QLabel(QStringLiteral("Code View connects with <tt>ssh HOST codeviewd bridge</tt>, using your SSH keys. "
                                           "codeviewd must be installed on the host."));
    note->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *form = new QFormLayout(&dlg);
    form->addRow(QStringLiteral("SSH target:"), target);
    form->addRow(QStringLiteral("Name:"), label);
    form->addRow(note);
    form->addRow(buttons);
    dlg.resize(480, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted) return;

    const QString t = target->currentText().trimmed();
    if (t.isEmpty()) return;
    const QString id = QStringLiteral("ssh:") + t;
    if (Connection::find(id)) {
        QMessageBox::information(this, QStringLiteral("Add Remote Host"), QStringLiteral("%1 is already added.").arg(t));
        return;
    }
    m_newHosts.insert(id);
    addConnection(id, label->text().trimmed().isEmpty() ? t : label->text().trimmed(), t);
    saveHosts();
    updateConnectionLabel();
}

void MainWindow::removeHost(Connection *conn)
{
    if (QMessageBox::question(this, QStringLiteral("Remove Host"),
                              QStringLiteral("Remove %1? Its sessions keep running on the host and come back if you add it again.")
                                  .arg(conn->label())) != QMessageBox::Yes)
        return;
    for (auto it = m_chats.begin(); it != m_chats.end();) {
        if (it.value()->connection() == conn) {
            m_sessionBar->removeEntry(it.key());
            m_terminals->removeSession(it.key());
            delete it.value();
            it = m_chats.erase(it);
        } else {
            ++it;
        }
    }
    if (m_files->connection() == conn) m_files->setRoot(nullptr, QString());
    if (m_login && m_login->connection() == conn) m_login->reject();
    m_conns.removeOne(conn);
    m_auth.remove(conn->id());
    delete conn;
    saveHosts();
    m_sessionStack->setCurrentIndex(m_chats.isEmpty() ? 0 : 1);
    updateConnectionLabel();
    currentSessionChanged();
}

void MainWindow::onConnectionState(Connection *conn)
{
    updateConnectionLabel();
    if (!conn->connected()) {
        if (m_newHosts.remove(conn->id())) {
            QString text = QStringLiteral("Couldn't connect to <b>%1</b>:<pre>%2</pre>")
                               .arg(conn->label().toHtmlEscaped(), conn->detail().toHtmlEscaped());
            if (const QString hint = connectionHint(conn->detail()); !hint.isEmpty())
                text += QString(hint).replace(QLatin1String("HOST"), conn->sshTarget().toHtmlEscaped());
            text += QStringLiteral("<p>Code View will keep retrying in the background.</p>");
            QMessageBox::warning(this, QStringLiteral("Remote Host"), text);
        }
        return;
    }
    m_newHosts.remove(conn->id());
    // Reload transcripts: events may have been missed while disconnected.
    for (ChatView *c : std::as_const(m_chats))
        if (c->connection() == conn) c->loadHistory();
    conn->request(QStringLiteral("list_sessions"), {}, [this, id = conn->id()](bool ok, const QJsonValue &data, const QString &) {
        if (Connection *c = Connection::find(id); c && ok) syncSessions(c, data.toArray());
    });
    if (m_files->connection() == conn) m_files->refresh();
    refreshAuth(conn, true);
}

void MainWindow::updateConnectionLabel()
{
    QStringList parts;
    QStringList tips;
    for (Connection *conn : std::as_const(m_conns)) {
        const QString name = conn->isLocal() && conn->connected() ? conn->hostname() : conn->label();
        if (conn->connected()) {
            parts << QStringLiteral("<span style='color:#27ae60'>●</span> %1").arg(name.toHtmlEscaped());
            tips << QStringLiteral("%1: connected").arg(name);
        } else {
            parts << QStringLiteral("<span style='color:#c0392b'>●</span> %1").arg(name.toHtmlEscaped());
            tips << QStringLiteral("%1: %2").arg(name, conn->detail().isEmpty() ? QStringLiteral("connecting…") : conn->detail());
        }
    }
    m_connLabel->setText(parts.join(QStringLiteral("&nbsp;&nbsp;&nbsp;")));
    m_connLabel->setToolTip(tips.join(QLatin1Char('\n')));
    if (Connection *c = currentConnection()) setWindowTitle(c->hostname()); // Qt appends the app name
}

void MainWindow::onEvent(Connection *conn, const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("sessions_changed")) {
        syncSessions(conn, event.value(QStringLiteral("sessions")).toArray());
    } else if (type == QLatin1String("session")) {
        if (ChatView *c = m_chats.value(chatKey(conn, event.value(QStringLiteral("session")).toString())))
            c->handleEvent(event.value(QStringLiteral("seq")).toInteger(), event.value(QStringLiteral("event")).toObject());
    } else if (type == QLatin1String("login_output")) {
        if (m_login && m_login->connection() == conn) m_login->appendOutput(event.value(QStringLiteral("text")).toString());
    } else if (type == QLatin1String("login_finished")) {
        if (m_login && m_login->connection() == conn) m_login->finished(event.value(QStringLiteral("success")).toBool());
        refreshAuth(conn);
    }
}

// ---- sessions ----

void MainWindow::syncSessions(Connection *conn, const QJsonArray &sessions)
{
    QSet<QString> seen;
    for (const QJsonValue &v : sessions) {
        const QJsonObject info = v.toObject();
        const QString key = chatKey(conn, info.value(QStringLiteral("id")).toString());
        seen.insert(key);
        ChatView *chat = m_chats.value(key);
        if (!chat) {
            chat = new ChatView(conn, info);
            chat->onOpenFile = [this, chat](const QString &p) { m_editors->openFile(chat->connection(), p); };
            chat->onFileChanged = [this, chat](const QString &p) {
                m_editors->reloadIfUnmodified(chat->connection(), p);
                if (chat == currentChat()) m_changes->scheduleRefresh();
            };
            chat->onTouchedChanged = [this, chat] {
                if (chat == currentChat()) m_changes->setClaudeFiles(chat->touchedFiles());
            };
            chat->onAuthProblem = [this, chat] { refreshAuth(chat->connection(), true); };
            m_chats.insert(key, chat);
            m_chatStack->addWidget(chat);
            chat->loadHistory();
        } else {
            chat->setInfo(info);
        }
        updateSessionEntry(chat);
        if (key == m_selectOnCreate || m_sessionBar->currentKey().isEmpty()) {
            if (key == m_selectOnCreate) chat->focusPrompt();
            m_selectOnCreate.clear();
            m_sessionBar->setCurrentKey(key);
        }
    }
    for (auto it = m_chats.begin(); it != m_chats.end();) {
        if (it.value()->connection() == conn && !seen.contains(it.key())) {
            m_sessionBar->removeEntry(it.key());
            m_terminals->removeSession(it.key());
            delete it.value();
            it = m_chats.erase(it);
        } else {
            ++it;
        }
    }
    m_sessionStack->setCurrentIndex(m_chats.isEmpty() ? 0 : 1);
    currentSessionChanged();
}

void MainWindow::updateSessionEntry(ChatView *chat)
{
    Connection *conn = chat->connection();
    m_sessionBar->setEntry(chatKey(conn, chat->id()), chat->name(), conn->isLocal() ? QString() : conn->label(),
                           conn->isLocal() ? chat->cwd() : conn->label() + QLatin1Char(':') + chat->cwd(), chat->status());
    if (chat == currentChat()) {
        updateChatTitle();
        if (chat->status() == QLatin1String("idle")) m_changes->scheduleRefresh(); // a turn may have just finished
    }
}

void MainWindow::updateChatTitle()
{
    ChatView *c = currentChat();
    m_chatDock->setWindowTitle(c ? QStringLiteral("Claude — %1").arg(c->name()) : QStringLiteral("Claude"));
}

ChatView *MainWindow::currentChat() const { return m_chats.value(m_sessionBar->currentKey()); }

Connection *MainWindow::currentConnection() const
{
    if (ChatView *c = currentChat()) return c->connection();
    return m_conns.isEmpty() ? nullptr : m_conns.first();
}

void MainWindow::currentSessionChanged()
{
    ChatView *c = currentChat();
    if (c) m_files->setRoot(c->connection(), c->cwd());
    m_changes->setSession(c ? c->connection() : nullptr, c ? c->cwd() : QString());
    m_changes->setClaudeFiles(c ? c->touchedFiles() : QStringList());
    m_terminals->setSession(c ? chatKey(c->connection(), c->id()) : QString(), c ? c->connection() : nullptr,
                            c ? c->cwd() : QString());
    updateChatTitle();
    updateAuthLabel();
    updateConnectionLabel();
}

void MainWindow::newSession(Connection *preferred)
{
    if (!preferred) preferred = currentConnection();
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("New Session"));
    auto *host = new QComboBox;
    for (Connection *conn : std::as_const(m_conns))
        if (conn->connected()) host->addItem(conn->isLocal() ? QStringLiteral("This computer (%1)").arg(conn->hostname()) : conn->label(), conn->id());
    if (host->count() == 0) {
        QMessageBox::warning(this, QStringLiteral("New Session"), QStringLiteral("No host is connected."));
        return;
    }
    if (preferred) host->setCurrentIndex(qMax(0, host->findData(preferred->id())));
    auto selectedConn = [host] { return Connection::find(host->currentData().toString()); };

    auto *name = new QLineEdit;
    name->setPlaceholderText(QStringLiteral("Defaults to the folder name"));
    auto *dir = new QLineEdit;
    auto defaultDir = [this, selectedConn] {
        Connection *conn = selectedConn();
        if (ChatView *c = currentChat(); c && c->connection() == conn) return c->cwd();
        return conn && !conn->home().isEmpty() ? conn->home() : QDir::homePath();
    };
    dir->setText(defaultDir());
    connect(host, &QComboBox::currentIndexChanged, &dlg, [dir, defaultDir] { dir->setText(defaultDir()); });
    auto *browse = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")), QString());
    connect(browse, &QPushButton::clicked, &dlg, [&dlg, dir, selectedConn] {
        Connection *conn = selectedConn();
        if (!conn) return;
        const QString p = pickFolder(&dlg, conn, dir->text());
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
    form->addRow(QStringLiteral("Host:"), host);
    form->addRow(QStringLiteral("Name:"), name);
    form->addRow(QStringLiteral("Folder:"), dirRow);
    form->addRow(QStringLiteral("Permissions:"), mode);
    form->addRow(buttons);
    dlg.resize(560, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted) return;

    Connection *conn = selectedConn();
    if (!conn) return;
    QJsonObject args{{QStringLiteral("name"), name->text().trimmed()}, {QStringLiteral("cwd"), dir->text().trimmed()}};
    if (!mode->currentData().toString().isEmpty()) args.insert(QStringLiteral("permission_mode"), mode->currentData().toString());
    conn->request(QStringLiteral("create_session"), args, [this, id = conn->id()](bool ok, const QJsonValue &data, const QString &error) {
        Connection *c = Connection::find(id);
        if (!c) return;
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("New Session"), error);
            return;
        }
        const QString key = chatKey(c, data[QStringLiteral("id")].toString());
        if (ChatView *chat = m_chats.value(key)) {
            m_sessionBar->setCurrentKey(key);
            chat->focusPrompt();
        } else {
            m_selectOnCreate = key; // the sessions_changed event hasn't arrived yet
        }
    });
}

void MainWindow::renameSession(ChatView *c)
{
    if (!c) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Session"), QStringLiteral("Name:"),
                                               QLineEdit::Normal, c->name(), &ok);
    if (ok && !name.trimmed().isEmpty())
        c->connection()->request(QStringLiteral("rename_session"),
                                 {{QStringLiteral("session"), c->id()}, {QStringLiteral("name"), name.trimmed()}});
}

void MainWindow::closeSession(ChatView *c)
{
    if (!c) return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Close Session"),
        QStringLiteral("Close “%1”? Claude stops, but the conversation stays in Claude's history (claude --resume).").arg(c->name()));
    if (answer == QMessageBox::Yes)
        c->connection()->request(QStringLiteral("close_session"), {{QStringLiteral("session"), c->id()}});
}

// ---- auth ----

void MainWindow::refreshAuth(Connection *conn, bool promptIfLoggedOut)
{
    if (!conn) return;
    conn->request(QStringLiteral("auth_status"), {}, [this, id = conn->id(), promptIfLoggedOut](bool ok, const QJsonValue &data, const QString &error) {
        Connection *c = Connection::find(id);
        if (!c) return;
        m_auth.insert(id, ok ? data.toObject() : QJsonObject{{QStringLiteral("error"), error}});
        updateAuthLabel();
        if (!ok) return;
        if (data[QStringLiteral("loggedIn")].toBool()) {
            m_authPrompted.remove(id);
            return;
        }
        if (promptIfLoggedOut && !m_authPrompted.contains(id) && !m_login) {
            m_authPrompted.insert(id);
            if (QMessageBox::question(this, QStringLiteral("Claude"),
                                      QStringLiteral("Claude isn't logged in on %1. Log in now?").arg(c->hostname())) == QMessageBox::Yes)
                openLogin(c);
        }
    });
}

void MainWindow::updateAuthLabel()
{
    Connection *conn = currentConnection();
    if (!conn || !m_auth.contains(conn->id())) {
        m_authLabel->clear();
        m_loginButton->hide();
        return;
    }
    const QJsonObject auth = m_auth.value(conn->id());
    const QString host = conn->isLocal() ? QString() : QStringLiteral(" on %1").arg(conn->label());
    if (auth.contains(QStringLiteral("loggedIn")) && auth.value(QStringLiteral("loggedIn")).toBool()) {
        const QString who = auth.value(QStringLiteral("email")).toString();
        m_authLabel->setText(QStringLiteral("Claude%1: %2").arg(host, who.isEmpty() ? QStringLiteral("logged in") : who));
        m_loginButton->hide();
    } else if (auth.contains(QStringLiteral("loggedIn"))) {
        m_authLabel->setText(QStringLiteral("Claude%1: not logged in").arg(host));
        m_loginButton->show();
    } else {
        m_authLabel->setText(QStringLiteral("Claude%1: unknown (%2)").arg(host, auth.value(QStringLiteral("error")).toString()));
        m_loginButton->hide();
    }
}

void MainWindow::openLogin(Connection *conn)
{
    if (!conn) return;
    if (m_login && m_login->connection() != conn) m_login->reject();
    if (!m_login) {
        m_login = new LoginDialog(conn, this);
        m_login->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_login->show();
    m_login->raise();
    m_login->start();
}

void MainWindow::changeEvent(QEvent *e)
{
    // Coming back from a terminal or another editor: git state may have changed.
    if (e->type() == QEvent::ActivationChange && isActiveWindow()) m_changes->scheduleRefresh();
    QMainWindow::changeEvent(e);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    // Sessions keep running in the daemons; only unsaved editor changes need asking about.
    if (m_editors->queryCloseAll()) e->accept();
    else e->ignore();
}

} // namespace cv
