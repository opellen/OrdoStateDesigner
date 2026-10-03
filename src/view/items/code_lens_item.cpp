#include "view/items/code_lens_item.h"

#include <algorithm>
#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QSet>

#include "constants/design_tokens.h"

namespace app {

namespace {

constexpr qreal kHeaderHeight = 28.0;
constexpr qreal kPaddingX = 14.0;
constexpr qreal kPaddingY = 10.0;
constexpr qreal kCornerRadius = 8.0;
constexpr qreal kMaxLensWidth = 460.0;
constexpr qreal kMinLensWidth = 280.0;
constexpr qreal kMaxLensHeight = 340.0;

// Syntax color tokens
QColor lensBackgroundColor() { return design::color(design::kLensFill); }
QColor lensHeaderBackgroundColor() { return design::color(design::kLensHeaderFill); }
QColor lensBorderColor() { return design::color(design::kAccentInteractiveSoft); }
QColor lensHeaderTextColor() { return design::color(design::kCodeIdentifier); }
QColor lensCloseButtonColor() { return design::color(design::kLensClose); }
QColor lensCloseHoverColor() { return design::color(design::kLensCloseHover); }

struct CodeToken {
    QString text;
    QColor color;
};

QVector<CodeToken> tokenizeCodeLine(const QString& line) {
    QVector<CodeToken> tokens;
    if (line.trimmed().startsWith(QStringLiteral("//"))) {
        tokens.append({line, design::color(design::kCodeComment)});  // Comments
        return tokens;
    }

    int i = 0;
    const int len = line.size();
    while (i < len) {
        const QChar ch = line[i];
        if (ch.isSpace()) {
            const int start = i++;
            while (i < len && line[i].isSpace()) ++i;
            // Whitespace takes the identifier color.
            tokens.append({line.mid(start, i - start), design::color(design::kCodeIdentifier)});
        } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            const int start = i++;
            while (i < len && line[i] != quote) {
                if (line[i] == QLatin1Char('\\') && i + 1 < len) {
                    i += 2;
                } else {
                    ++i;
                }
            }
            if (i < len) ++i;
            tokens.append({line.mid(start, i - start), design::color(design::kCodeString)});  // String
        } else if (ch.isDigit()) {
            const int start = i++;
            while (i < len && (line[i].isDigit() || line[i] == QLatin1Char('.'))) ++i;
            tokens.append({line.mid(start, i - start), design::color(design::kCodeNumber)});  // Number
        } else if (ch.isLetter() || ch == QLatin1Char('_')) {
            const int start = i++;
            while (i < len &&
                   (line[i].isLetterOrNumber() || line[i] == QLatin1Char('_') || line[i] == QLatin1Char(':'))) {
                if (line[i] == QLatin1Char(':') && (i + 1 >= len || line[i + 1] != QLatin1Char(':'))) {
                    break;
                }
                ++i;
            }
            const QString word = line.mid(start, i - start);
            static const QSet<QString> keywords = {
                QStringLiteral("void"),      QStringLiteral("bool"),     QStringLiteral("int"),
                QStringLiteral("double"),    QStringLiteral("case"),     QStringLiteral("break"),
                QStringLiteral("return"),    QStringLiteral("if"),       QStringLiteral("else"),
                QStringLiteral("switch"),    QStringLiteral("virtual"),  QStringLiteral("override"),
                QStringLiteral("const"),     QStringLiteral("class"),    QStringLiteral("struct"),
                QStringLiteral("enum"),      QStringLiteral("true"),     QStringLiteral("false"),
                QStringLiteral("public"),    QStringLiteral("private"),  QStringLiteral("namespace")};
            if (keywords.contains(word)) {
                tokens.append({word, design::color(design::kCodeKeyword)});  // Keyword
            } else if (word.contains(QStringLiteral("::")) || word.endsWith(QStringLiteral("State")) ||
                       word == QStringLiteral("Context") || word == QStringLiteral("Event") ||
                       word == QStringLiteral("Machine")) {
                tokens.append({word, design::color(design::kCodeType)});  // Type/Enum
            } else {
                tokens.append({word, design::color(design::kCodeIdentifier)});  // Identifier
            }
        } else {
            const int start = i++;
            while (i < len && !line[i].isLetterOrNumber() && line[i] != QLatin1Char('_') && !line[i].isSpace() &&
                   line[i] != QLatin1Char('"') && line[i] != QLatin1Char('\'')) {
                ++i;
            }
            tokens.append({line.mid(start, i - start), design::color(design::kCodePunctuation)});  // Punctuation
        }
    }
    return tokens;
}

}  // namespace

CodeLensItem::CodeLensItem() {
    setZValue(60.0);  // Above nodes (10), labels (20), and action box (50)
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setVisible(false);

    headerFont_.setPointSize(9);
    headerFont_.setBold(true);

    codeFont_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont_.setPointSize(8);

    auto* shadow = new QGraphicsDropShadowEffect();
    shadow->setBlurRadius(20.0);
    shadow->setColor(design::color(design::kShadow));
    shadow->setOffset(0.0, 4.0);
    setGraphicsEffect(shadow);
}

void CodeLensItem::showLens(const QString& title, const QString& code, const QRectF& anchorRect) {
    title_ = title;
    code_ = code;
    lines_ = code.split(QLatin1Char('\n'));
    layoutLens(anchorRect);
    setVisible(true);
    update();
}

void CodeLensItem::hideLens() {
    if (isVisible()) {
        setVisible(false);
        update();
    }
}

void CodeLensItem::layoutLens(const QRectF& anchorRect) {
    prepareGeometryChange();

    const QFontMetricsF codeMetrics(codeFont_);
    qreal maxLineWidth = 0.0;
    for (const QString& line : lines_) {
        maxLineWidth = std::max(maxLineWidth, codeMetrics.horizontalAdvance(line));
    }

    const qreal width = std::clamp(maxLineWidth + kPaddingX * 2.0, kMinLensWidth, kMaxLensWidth);
    const qreal contentHeight = lines_.size() * codeMetrics.height() + kPaddingY * 2.0;
    const qreal height = std::min(kHeaderHeight + contentHeight, kMaxLensHeight);

    rect_ = QRectF(0.0, 0.0, width, height);
    closeButtonRect_ = QRectF(width - 26.0, 4.0, 20.0, 20.0);

    // Position adjacent to the anchor
    const qreal posX = anchorRect.right() + 16.0;
    const qreal posY = anchorRect.top();
    setPos(posX, posY);
}

QRectF CodeLensItem::boundingRect() const {
    return rect_.adjusted(-2.0, -2.0, 2.0, 2.0);
}

void CodeLensItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);

    // Card background
    painter->setPen(QPen(lensBorderColor(), 1.5));
    painter->setBrush(QBrush(lensBackgroundColor()));
    painter->drawRoundedRect(rect_, kCornerRadius, kCornerRadius);

    // Header band
    QPainterPath headerClip;
    headerClip.addRoundedRect(rect_, kCornerRadius, kCornerRadius);
    painter->save();
    painter->setClipPath(headerClip);
    painter->fillRect(QRectF(0.0, 0.0, rect_.width(), kHeaderHeight), lensHeaderBackgroundColor());

    // Separator line
    painter->setPen(QPen(design::color(design::kLensSeparator), 1.0));
    painter->drawLine(QPointF(0.0, kHeaderHeight), QPointF(rect_.width(), kHeaderHeight));

    // Header Title
    painter->setFont(headerFont_);
    painter->setPen(lensHeaderTextColor());
    const QRectF titleRect(kPaddingX, 0.0, rect_.width() - kPaddingX * 2.0 - 24.0, kHeaderHeight);
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Code Lens (C++): %1").arg(title_));

    // Close button
    if (closeHovered_) {
        painter->setBrush(design::color(design::kLensCloseHoverFill));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(closeButtonRect_, 4.0, 4.0);
    }
    QPen closePen(closeHovered_ ? lensCloseHoverColor() : lensCloseButtonColor(), 1.5);
    closePen.setCapStyle(Qt::RoundCap);
    painter->setPen(closePen);
    const QRectF btn = closeButtonRect_.adjusted(6.0, 6.0, -6.0, -6.0);
    painter->drawLine(btn.topLeft(), btn.bottomRight());
    painter->drawLine(btn.topRight(), btn.bottomLeft());
    painter->restore();

    // Code area
    painter->save();
    painter->setFont(codeFont_);
    const QFontMetricsF codeMetrics(codeFont_);
    const QRectF codeBounds(kPaddingX, kHeaderHeight + kPaddingY, rect_.width() - kPaddingX * 2.0,
                            rect_.height() - kHeaderHeight - kPaddingY);
    painter->setClipRect(codeBounds);

    qreal curY = kHeaderHeight + kPaddingY;
    for (const QString& line : lines_) {
        if (curY + codeMetrics.height() > rect_.bottom()) {
            break;  // Truncate at max card height
        }
        const auto tokens = tokenizeCodeLine(line);
        qreal curX = kPaddingX;
        for (const auto& token : tokens) {
            const qreal tokenWidth = codeMetrics.horizontalAdvance(token.text);
            painter->setPen(token.color);
            painter->drawText(QRectF(curX, curY, tokenWidth + 1.0, codeMetrics.height()), Qt::AlignLeft | Qt::AlignTop,
                              token.text);
            curX += tokenWidth;
        }
        curY += codeMetrics.height();
    }
    painter->restore();
}

void CodeLensItem::setOnCloseClicked(std::function<void()> cb) {
    onCloseClicked_ = std::move(cb);
}

void CodeLensItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::LeftButton && closeButtonRect_.contains(event->pos())) {
        hideLens();
        if (onCloseClicked_) {
            onCloseClicked_();
        }
        event->accept();
        return;
    }
    QGraphicsItem::mousePressEvent(event);
}

void CodeLensItem::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    const bool wasHovered = closeHovered_;
    closeHovered_ = closeButtonRect_.contains(event->pos());
    if (closeHovered_ != wasHovered) {
        setCursor(closeHovered_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update(closeButtonRect_);
    }
    QGraphicsItem::hoverMoveEvent(event);
}

void CodeLensItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    if (closeHovered_) {
        closeHovered_ = false;
        unsetCursor();
        update(closeButtonRect_);
    }
    QGraphicsItem::hoverLeaveEvent(event);
}

}  // namespace app
