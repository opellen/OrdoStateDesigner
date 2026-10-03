#pragma once

#include <QProgressBar>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace app {

class ProgressOverlayWidget : public QWidget {
    Q_OBJECT

public:
    explicit ProgressOverlayWidget(QWidget* parent = nullptr);
    ~ProgressOverlayWidget() override = default;

    void showOperation(quint64 opId, const QString& title, const QString& message, bool cancelable = true);
    void setProgress(int percentage, const QString& message);
    void hideOperation();

    quint64 currentOpId() const { return currentOpId_; }
    bool isCancelable() const { return cancelBtn_ && cancelBtn_->isVisible(); }

signals:
    void cancelRequested(quint64 opId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateCardGeometry();

    quint64 currentOpId_ = 0;
    QString title_;
    QString statusMessage_;
    int percentage_ = -1;
    int spinnerAngle_ = 0;
    QTimer spinnerTimer_;

    QWidget* card_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
};

}  // namespace app
