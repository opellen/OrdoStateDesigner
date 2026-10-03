#include "view/shell/progress_overlay_widget.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include "constants/design_tokens.h"

namespace app {

namespace {

class SpinnerWidget : public QWidget {
public:
    explicit SpinnerWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(48, 48);
    }

    void setAngle(int angle) {
        angle_ = angle;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF rect(6, 6, 36, 36);
        // Dim background track (outline-strong)
        QPen trackPen(design::color(design::kOutlineStrong), 4, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(trackPen);
        painter.drawEllipse(rect);

        // Active rotating arc (accent-pulse: transient live/run emphasis)
        QPen activePen(design::color(design::kAccentPulse), 4, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(activePen);
        painter.drawArc(rect, -angle_ * 16, 90 * 16);
    }

private:
    int angle_ = 0;
};
}  // namespace

ProgressOverlayWidget::ProgressOverlayWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_NoMousePropagation);
    setFocusPolicy(Qt::StrongFocus);
    hide();

    if (parent != nullptr) {
        parent->installEventFilter(this);
        resize(parent->size());
    }

    // Centered card container
    card_ = new QWidget(this);
    card_->setObjectName(QStringLiteral("progressCard"));
    card_->setStyleSheet(design::resolveRoles(QStringLiteral(
        "#progressCard {"
        "  background-color: {surface-2};"
        "  border: 1px solid {outline-strong};"
        "  border-radius: 10px;"
        "}"
        "QLabel {"
        "  color: {text-primary};"
        "}"
        "QProgressBar {"
        "  border: 1px solid {outline-strong};"
        "  border-radius: 4px;"
        "  background-color: {surface-2};"
        "  text-align: center;"
        "  color: {text-primary};"
        "  height: 14px;"
        "}"
        "QProgressBar::chunk {"
        "  background-color: {accent-pulse};"
        "  border-radius: 3px;"
        "}"
        "QPushButton {"
        "  background-color: {surface-2};"
        "  color: {text-primary};"
        "  border: 1px solid {outline-hover};"
        "  border-radius: 4px;"
        "  padding: 4px 14px;"
        "  font-size: 12px;"
        "}"
        "QPushButton:hover {"
        "  background-color: {surface-hover};"
        "}"
        "QPushButton:pressed {"
        "  background-color: {surface-pressed};"
        "}"
    )));

    auto* cardLayout = new QVBoxLayout(card_);
    cardLayout->setContentsMargins(24, 20, 24, 20);
    cardLayout->setSpacing(12);

    auto* topLayout = new QHBoxLayout();
    auto* spinner = new SpinnerWidget(card_);
    topLayout->addWidget(spinner);

    auto* textLayout = new QVBoxLayout();
    auto* titleLabel = new QLabel(QStringLiteral("Operation in progress"), card_);
    titleLabel->setObjectName(QStringLiteral("titleLabel"));
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(12);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    textLayout->addWidget(titleLabel);

    auto* statusLabel = new QLabel(QStringLiteral("Please wait..."), card_);
    statusLabel->setObjectName(QStringLiteral("statusLabel"));
    statusLabel->setStyleSheet(design::resolveRoles(QStringLiteral("color: {text-secondary}; font-size: 11px;")));
    textLayout->addWidget(statusLabel);

    topLayout->addLayout(textLayout);
    topLayout->addStretch();
    cardLayout->addLayout(topLayout);

    progressBar_ = new QProgressBar(card_);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(true);
    cardLayout->addWidget(progressBar_);

    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->addStretch();
    cancelBtn_ = new QPushButton(QStringLiteral("Cancel"), card_);
    bottomLayout->addWidget(cancelBtn_);
    cardLayout->addLayout(bottomLayout);

    card_->setFixedSize(380, 160);

    connect(cancelBtn_, &QPushButton::clicked, this, [this]() {
        emit cancelRequested(currentOpId_);
    });

    connect(&spinnerTimer_, &QTimer::timeout, this, [this, spinner]() {
        spinnerAngle_ = (spinnerAngle_ + 8) % 360;
        spinner->setAngle(spinnerAngle_);
    });
}

void ProgressOverlayWidget::showOperation(quint64 opId, const QString& title, const QString& message, bool cancelable) {
    currentOpId_ = opId;
    title_ = title;
    statusMessage_ = message;
    percentage_ = -1;

    auto* titleLabel = card_->findChild<QLabel*>(QStringLiteral("titleLabel"));
    if (titleLabel != nullptr) {
        titleLabel->setText(title_);
    }

    auto* statusLabel = card_->findChild<QLabel*>(QStringLiteral("statusLabel"));
    if (statusLabel != nullptr) {
        statusLabel->setText(statusMessage_);
    }

    progressBar_->setRange(0, 0);  // Indeterminate by default
    cancelBtn_->setVisible(cancelable);

    if (parentWidget() != nullptr) {
        resize(parentWidget()->size());
        updateCardGeometry();
    }

    raise();
    show();
    spinnerTimer_.start(25);
}

void ProgressOverlayWidget::setProgress(int percentage, const QString& message) {
    percentage_ = percentage;
    if (!message.isEmpty()) {
        statusMessage_ = message;
        auto* statusLabel = card_->findChild<QLabel*>(QStringLiteral("statusLabel"));
        if (statusLabel != nullptr) {
            statusLabel->setText(statusMessage_);
        }
    }

    if (percentage >= 0) {
        progressBar_->setRange(0, 100);
        progressBar_->setValue(percentage);
    } else {
        progressBar_->setRange(0, 0);
    }
}

void ProgressOverlayWidget::hideOperation() {
    spinnerTimer_.stop();
    hide();
    currentOpId_ = 0;
}

void ProgressOverlayWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // Modal scrim behind the progress card
    painter.fillRect(rect(), design::color(design::kScrim));
}

void ProgressOverlayWidget::resizeEvent(QResizeEvent*) {
    updateCardGeometry();
}

bool ProgressOverlayWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        resize(parentWidget()->size());
        updateCardGeometry();
    }
    return QWidget::eventFilter(watched, event);
}

void ProgressOverlayWidget::updateCardGeometry() {
    if (card_ == nullptr) return;
    const int x = (width() - card_->width()) / 2;
    const int y = (height() - card_->height()) / 2;
    card_->move(std::max(0, x), std::max(0, y));
}

}  // namespace app
