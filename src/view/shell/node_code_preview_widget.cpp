#include "view/shell/node_code_preview_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTimer>
#include <QVBoxLayout>

#include "constants/design_tokens.h"

namespace app {

namespace {

class CppSyntaxHighlighter : public QSyntaxHighlighter {
public:
    explicit CppSyntaxHighlighter(QTextDocument* parent = nullptr) : QSyntaxHighlighter(parent) {
        HighlightingRule rule;

        // Keywords
        keywordFormat_.setForeground(design::color(design::kCodeKeyword));  // code-keyword
        keywordFormat_.setFontWeight(QFont::Bold);
        const QStringList keywords = {
            QStringLiteral("\\bcase\\b"),    QStringLiteral("\\bbreak\\b"),   QStringLiteral("\\breturn\\b"),
            QStringLiteral("\\bif\\b"),      QStringLiteral("\\belse\\b"),    QStringLiteral("\\bvirtual\\b"),
            QStringLiteral("\\bvoid\\b"),    QStringLiteral("\\bbool\\b"),    QStringLiteral("\\bconst\\b"),
            QStringLiteral("\\btrue\\b"),    QStringLiteral("\\bfalse\\b"),   QStringLiteral("\\bstruct\\b"),
            QStringLiteral("\\bclass\\b"),   QStringLiteral("\\benum\\b"),    QStringLiteral("\\bauto\\b"),
            QStringLiteral("\\bswitch\\b"),  QStringLiteral("\\bdefault\\b"),
        };
        for (const QString& pattern : keywords) {
            rule.pattern = QRegularExpression(pattern);
            rule.format = keywordFormat_;
            highlightingRules_.append(rule);
        }

        // Types
        typeFormat_.setForeground(design::color(design::kCodeType));  // code-type
        const QStringList types = {
            QStringLiteral("\\bContext\\b"),         QStringLiteral("\\bState\\b"),
            QStringLiteral("\\bstd::stop_token\\b"),  QStringLiteral("\\bstd::function\\b"),
            QStringLiteral("\\bstd::string\\b"),     QStringLiteral("\\bint\\b"),
            QStringLiteral("\\bdouble\\b"),          QStringLiteral("\\bfloat\\b"),
            QStringLiteral("\\bquint64\\b"),         QStringLiteral("\\bstd::uint32_t\\b"),
        };
        for (const QString& pattern : types) {
            rule.pattern = QRegularExpression(pattern);
            rule.format = typeFormat_;
            highlightingRules_.append(rule);
        }

        // Comments
        commentFormat_.setForeground(design::color(design::kCodeComment));  // code-comment
        rule.pattern = QRegularExpression(QStringLiteral("//[^\n]*"));
        rule.format = commentFormat_;
        highlightingRules_.append(rule);

        // Strings
        stringFormat_.setForeground(design::color(design::kCodeString));  // code-string
        rule.pattern = QRegularExpression(QStringLiteral("\".*\""));
        rule.format = stringFormat_;
        highlightingRules_.append(rule);
    }

protected:
    void highlightBlock(const QString& text) override {
        for (const HighlightingRule& rule : highlightingRules_) {
            QRegularExpressionMatchIterator matchIterator = rule.pattern.globalMatch(text);
            while (matchIterator.hasNext()) {
                QRegularExpressionMatch match = matchIterator.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.format);
            }
        }
    }

private:
    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<HighlightingRule> highlightingRules_;
    QTextCharFormat keywordFormat_;
    QTextCharFormat typeFormat_;
    QTextCharFormat commentFormat_;
    QTextCharFormat stringFormat_;
};

}  // namespace

NodeCodePreviewWidget::NodeCodePreviewWidget(bool isTransition, QWidget* parent)
    : QWidget(parent), isTransition_(isTransition) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 8, 0, 0);
    mainLayout->setSpacing(4);

    // Section Header: Title + Copy Button
    auto* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);

    auto* titleLabel = new QLabel(QStringLiteral("Code Projection (C++)"), this);
    titleLabel->setStyleSheet(design::resolveRoles(QStringLiteral("QLabel { color: {text-secondary}; font-size: 11px; font-weight: bold; }")));
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    copyBtn_ = new QPushButton(QStringLiteral("Copy"), this);
    copyBtn_->setCursor(Qt::PointingHandCursor);
    copyBtn_->setStyleSheet(design::resolveRoles(
        QStringLiteral("QPushButton { background: {surface-2}; color: {text-primary}; border: 1px solid {outline-strong}; "
                       "border-radius: 3px; font-size: 11px; padding: 2px 8px; } "
                       "QPushButton:hover { background: {surface-hover}; } "
                       "QPushButton:pressed { background: {surface-pressed}; }")));
    connect(copyBtn_, &QPushButton::clicked, this, &NodeCodePreviewWidget::onCopyClicked);
    headerLayout->addWidget(copyBtn_);

    mainLayout->addLayout(headerLayout);

    // Filter Buttons Bar
    auto* filterLayout = new QHBoxLayout();
    filterLayout->setContentsMargins(0, 2, 0, 2);
    filterLayout->setSpacing(2);

    const QString btnStyle = design::resolveRoles(QStringLiteral(
        "QPushButton { background: {surface-2}; color: {text-secondary}; border: 1px solid {outline-strong}; "
        "border-radius: 3px; font-size: 10px; padding: 2px 6px; } "
        "QPushButton:hover { color: {text-primary}; background: {surface-hover}; } "
        "QPushButton:checked { background: {surface-hover}; color: {accent-interactive}; font-weight: bold; border-color: {accent-interactive}; }"));

    allBtn_ = new QPushButton(QStringLiteral("All"), this);
    allBtn_->setCheckable(true);
    allBtn_->setStyleSheet(btnStyle);
    connect(allBtn_, &QPushButton::clicked, this, [this] { debugSelectFilter(Filter::All); });
    filterLayout->addWidget(allBtn_);

    enumBtn_ = new QPushButton(isTransition_ ? QStringLiteral("Header") : QStringLiteral("State Enum"), this);
    enumBtn_->setCheckable(true);
    enumBtn_->setStyleSheet(btnStyle);
    connect(enumBtn_, &QPushButton::clicked, this, [this] { debugSelectFilter(Filter::StateEnum); });
    filterLayout->addWidget(enumBtn_);

    hooksBtn_ = new QPushButton(QStringLiteral("Hooks"), this);
    hooksBtn_->setCheckable(true);
    hooksBtn_->setStyleSheet(btnStyle);
    connect(hooksBtn_, &QPushButton::clicked, this, [this] { debugSelectFilter(Filter::Hooks); });
    filterLayout->addWidget(hooksBtn_);

    coreBtn_ = new QPushButton(QStringLiteral("Core"), this);
    coreBtn_->setCheckable(true);
    coreBtn_->setStyleSheet(btnStyle);
    connect(coreBtn_, &QPushButton::clicked, this, [this] { debugSelectFilter(Filter::CoreHandler); });
    filterLayout->addWidget(coreBtn_);

    filterLayout->addStretch();
    mainLayout->addLayout(filterLayout);

    // Code Editor Widget
    codeEdit_ = new QPlainTextEdit(this);
    codeEdit_->setReadOnly(true);
    codeEdit_->setMinimumHeight(140);
    codeEdit_->setMaximumHeight(260);

    QFont codeFont(QStringLiteral("Consolas"));
    codeFont.setStyleHint(QFont::Monospace);
    codeFont.setPointSize(9);
    codeEdit_->setFont(codeFont);

    codeEdit_->setStyleSheet(design::resolveRoles(
        QStringLiteral("QPlainTextEdit { background-color: {surface-2}; color: {text-primary}; "
                       "border: 1px solid {outline}; border-radius: 4px; padding: 4px; }")));
    new CppSyntaxHighlighter(codeEdit_->document());

    mainLayout->addWidget(codeEdit_);

    debugSelectFilter(Filter::All);
}

void NodeCodePreviewWidget::setProjection(const NodeCodeProjection& projection) {
    currentProjection_ = projection;
    updateDisplayedText();
}

void NodeCodePreviewWidget::clear() {
    currentProjection_ = {};
    codeEdit_->clear();
}

QString NodeCodePreviewWidget::currentSnippet() const {
    switch (activeFilter_) {
    case Filter::All:
        return currentProjection_.fullSnippet;
    case Filter::StateEnum:
        return currentProjection_.stateEnumSnippet;
    case Filter::Hooks:
        return currentProjection_.hooksSnippet;
    case Filter::CoreHandler:
        return currentProjection_.coreHandlerSnippet;
    }
    return {};
}

void NodeCodePreviewWidget::debugSelectFilter(Filter filter) {
    activeFilter_ = filter;
    allBtn_->setChecked(filter == Filter::All);
    enumBtn_->setChecked(filter == Filter::StateEnum);
    hooksBtn_->setChecked(filter == Filter::Hooks);
    coreBtn_->setChecked(filter == Filter::CoreHandler);
    updateDisplayedText();
}

QString NodeCodePreviewWidget::debugDisplayedText() const {
    return codeEdit_->toPlainText();
}

void NodeCodePreviewWidget::debugClickCopy() {
    onCopyClicked();
}

void NodeCodePreviewWidget::updateDisplayedText() {
    codeEdit_->setPlainText(currentSnippet());
}

void NodeCodePreviewWidget::onCopyClicked() {
    const QString text = currentSnippet();
    if (!text.isEmpty()) {
        QClipboard* cb = QApplication::clipboard();
        if (cb != nullptr) {
            cb->setText(text);
        }
    }
    copyBtn_->setText(QStringLiteral("Copied!"));
    QTimer::singleShot(1500, copyBtn_, [this] { copyBtn_->setText(QStringLiteral("Copy")); });
}

}  // namespace app
