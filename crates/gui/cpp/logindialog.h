#pragma once
#include <QDialog>
#include <functional>

class QPlainTextEdit;
class QLineEdit;
class QPushButton;
class QLabel;

namespace cv {

// Shows `claude auth login` running on the daemon's host and lets you open
// the login URL here and paste the resulting code back.
class LoginDialog : public QDialog
{
public:
    LoginDialog(const QString &host, QWidget *parent = nullptr);

    void start();
    void appendOutput(const QString &text);
    void finished(bool success);

    std::function<void()> onFinished;

protected:
    void reject() override;

private:
    QPlainTextEdit *m_output;
    QLineEdit *m_input;
    QPushButton *m_openUrl;
    QLabel *m_status;
    QString m_url;
    bool m_running = false;
};

} // namespace cv
