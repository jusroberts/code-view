#pragma once
#include <QDialog>
#include <functional>

class QPlainTextEdit;
class QLineEdit;
class QPushButton;
class QLabel;

namespace cv {

class Connection;

// Shows `claude auth login` running on the daemon's host and lets you open
// the login URL here and paste the resulting code back.
class LoginDialog : public QDialog
{
public:
    LoginDialog(Connection *conn, QWidget *parent = nullptr);

    Connection *connection() const { return m_conn; }

    void start();
    void appendOutput(const QString &text);
    void finished(bool success);

    std::function<void()> onFinished;

    // Also cancels a running login.
    void reject() override;

private:
    Connection *m_conn;
    QPlainTextEdit *m_output;
    QLineEdit *m_input;
    QPushButton *m_openUrl;
    QLabel *m_status;
    QString m_url;
    bool m_running = false;
};

} // namespace cv
