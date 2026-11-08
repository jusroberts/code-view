#include "logindialog.h"
#include "daemonclient.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QUrl>
#include <QVBoxLayout>

namespace cv {

LoginDialog::LoginDialog(const QString &host, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Log in to Claude on %1").arg(host));
    resize(720, 420);

    auto *intro = new QLabel(QStringLiteral(
        "Running <tt>claude auth login</tt> on <b>%1</b>. Open the login page, sign in, and if Claude shows you a code, "
        "paste it below and press Enter.").arg(host.toHtmlEscaped()));
    intro->setWordWrap(true);

    m_output = new QPlainTextEdit;
    m_output->setReadOnly(true);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_openUrl = new QPushButton(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open login page"));
    m_openUrl->setEnabled(false);
    connect(m_openUrl, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(m_url)); });

    m_input = new QLineEdit;
    m_input->setPlaceholderText(QStringLiteral("Paste the code here and press Enter (empty Enter sends Enter)"));
    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        DaemonClient::instance().request(QStringLiteral("login_input"), {{QStringLiteral("text"), m_input->text()}});
        m_input->clear();
    });

    m_status = new QLabel;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *row = new QHBoxLayout;
    row->addWidget(m_input, 1);
    row->addWidget(m_openUrl);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_output, 1);
    layout->addLayout(row);
    layout->addWidget(m_status);
    layout->addWidget(buttons);
}

void LoginDialog::start()
{
    m_output->clear();
    m_url.clear();
    m_openUrl->setEnabled(false);
    m_status->setText(QStringLiteral("Starting…"));
    m_running = true;
    QPointer<LoginDialog> self(this);
    DaemonClient::instance().request(QStringLiteral("start_login"), {}, [self](bool ok, const QJsonValue &, const QString &error) {
        if (!self) return;
        if (ok) {
            self->m_status->setText(QStringLiteral("Waiting for login…"));
            self->m_input->setFocus();
        } else {
            self->m_running = false;
            self->m_status->setText(QStringLiteral("Couldn't start login: %1").arg(error));
        }
    });
}

void LoginDialog::appendOutput(const QString &text)
{
    m_output->moveCursor(QTextCursor::End);
    m_output->insertPlainText(text);
    m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
    static const QRegularExpression urlRe(QStringLiteral(R"(https://\S+)"));
    auto it = urlRe.globalMatch(m_output->toPlainText());
    while (it.hasNext()) m_url = it.next().captured(0);
    m_openUrl->setEnabled(!m_url.isEmpty());
}

void LoginDialog::finished(bool success)
{
    m_running = false;
    m_status->setText(success ? QStringLiteral("✓ Logged in.") : QStringLiteral("Login didn't complete."));
    if (onFinished) onFinished();
}

void LoginDialog::reject()
{
    if (m_running) DaemonClient::instance().request(QStringLiteral("cancel_login"));
    m_running = false;
    QDialog::reject();
}

} // namespace cv
