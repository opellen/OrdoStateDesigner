#include "view/items/note_item.h"

#include <algorithm>
#include <cmath>

#include <QAbstractTextDocumentLayout>
#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPolygonF>
#include <QStyleOptionGraphicsItem>

#include "constants/design_tokens.h"
#include "view/geometry/element_colors.h"  // elementAccent
// Shared markdown list presentation, so note and editor render lists identically.
#include "view/canvas/note_editor_rules.h"

namespace app {

namespace {

// Text word-wraps at the max width; the box hugs it, floored so a short or
// empty note stays a clickable surface.
constexpr qreal kMaxWidth = 264.0;  // 11 grid units
constexpr qreal kMinWidth = 96.0;
constexpr qreal kMinHeight = 40.0;
constexpr qreal kCornerRadius = 6.0;
constexpr qreal kFoldSize = 14.0;
constexpr qreal kTextPadding = 10.0;

constexpr qreal kGridStep = 24.0;    // matches CanvasView's grid and StateItem's drag snap
constexpr qreal kSnapRadius = 4.0;   // matches StateItem's soft-snap radius

QColor textColor() { return design::color(design::kNoteText); }
QColor placeholderColor() { return design::color(design::kNotePlaceholder); }
QColor hoverFillColor() { return design::color(design::kNoteHoverFill); }
QColor hoverBorderColor() { return design::color(design::kCanvasHoverOverlay); }
QColor selectedBorderColor() { return design::color(design::kAccentInteractive); }
QColor foldColor() { return design::color(design::kNoteFold); }

// Kept short for the small at-rest box.
QString displayText(const QString& text) { return text.isEmpty() ? QStringLiteral("Type a note") : text; }

QFont noteFont() {
    QFont font;
    font.setPointSizeF(9.0);
    return font;
}

}  // namespace

NoteItem::NoteItem(quint64 id, QString text) : id_(id), text_(std::move(text)) {
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    // Under nodes (10), above edges (0).
    setZValue(5.0);
    recomputeGeometry();
}

void NoteItem::setText(const QString& text) {
    if (text_ == text) {
        return;
    }
    text_ = text;
    recomputeGeometry();  // the box follows the text
    update();
}

void NoteItem::recomputeGeometry() {
    prepareGeometryChange();
    if (text_.isEmpty()) {
        // Placeholder: plain font metrics.
        const QFontMetricsF metrics(noteFont());
        const QRectF textBound =
            metrics.boundingRect(QRectF(0.0, 0.0, kMaxWidth - kTextPadding * 2.0, 100000.0),
                                 Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, displayText(text_));
        const qreal width = std::clamp(textBound.width() + kTextPadding * 2.0, kMinWidth, kMaxWidth);
        const qreal height = std::max(textBound.height() + kTextPadding * 2.0, kMinHeight);
        rect_ = QRectF(0.0, 0.0, width, height);
        return;
    }
    // The document is laid out once here; geometry and paint both read it.
    // Width hugs the ideal (unwrapped) width up to kMaxWidth; color is paint-time only.
    doc_.setDefaultFont(noteFont());
    doc_.setDocumentMargin(0.0);
    doc_.setMarkdown(text_, QTextDocument::MarkdownDialectGitHub);
    note_editor_rules::applyListPresentation(&doc_);  // indent steps and depth bullets, same as the editor
    doc_.setTextWidth(-1.0);
    const qreal contentWidth = std::min(doc_.idealWidth(), kMaxWidth - kTextPadding * 2.0);
    doc_.setTextWidth(contentWidth);
    const qreal width = std::clamp(contentWidth + kTextPadding * 2.0, kMinWidth, kMaxWidth);
    const qreal height = std::max(doc_.size().height() + kTextPadding * 2.0, kMinHeight);
    rect_ = QRectF(0.0, 0.0, width, height);
}


void NoteItem::setColor(ElementColor color) {
    if (color_ == color) {
        return;
    }
    color_ = color;
    update();
}

QRectF NoteItem::sceneRect() const { return QRectF(pos(), rect_.size()); }

void NoteItem::setOnMoved(std::function<void()> cb) { onMoved_ = std::move(cb); }
void NoteItem::setOnDragStarted(std::function<void()> cb) { onDragStarted_ = std::move(cb); }
void NoteItem::setOnDragFinished(std::function<void(QPointF)> cb) { onDragFinished_ = std::move(cb); }
void NoteItem::setOnDragReverted(std::function<void()> cb) { onDragReverted_ = std::move(cb); }
void NoteItem::setOnEditRequested(std::function<void()> cb) { onEditRequested_ = std::move(cb); }

QRectF NoteItem::boundingRect() const { return rect_; }

QVariant NoteItem::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionChange && scene() != nullptr) {
        // Soft snap: only within kSnapRadius of a grid line, as in StateItem.
        QPointF newPos = value.toPointF();
        const qreal snappedX = std::round(newPos.x() / kGridStep) * kGridStep;
        const qreal snappedY = std::round(newPos.y() / kGridStep) * kGridStep;
        if (std::abs(newPos.x() - snappedX) <= kSnapRadius) {
            newPos.setX(snappedX);
        }
        if (std::abs(newPos.y() - snappedY) <= kSnapRadius) {
            newPos.setY(snappedY);
        }
        return newPos;
    }
    if (change == ItemPositionHasChanged && scene() != nullptr) {
        // Only a held left press opens a drag session, so a presenter-driven
        // setPos (undo/redo, echo) never does.
        if (leftPressed_ && !dragReported_ && pos() != dragStartPos_) {
            dragReported_ = true;
            if (onDragStarted_) {
                onDragStarted_();
            }
        }
        if (onMoved_) {
            onMoved_();
        }
        return value;
    }
    if (change == ItemSelectedHasChanged) {
        update();
        return value;
    }
    return QGraphicsItem::itemChange(change, value);
}

void NoteItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = true;
    setCursor(Qt::PointingHandCursor);
    update();
    QGraphicsItem::hoverEnterEvent(event);
}

void NoteItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = false;
    unsetCursor();
    update();
    QGraphicsItem::hoverLeaveEvent(event);
}

void NoteItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragStartPos_ = pos();
        leftPressed_ = true;
        dragReported_ = false;
        // Sampled before the base impl, whose press selects the item.
        wasSelectedAtPress_ = isSelected();
    }
    QGraphicsItem::mousePressEvent(event);
}

void NoteItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    QGraphicsItem::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const bool reported = dragReported_;
    leftPressed_ = false;
    dragReported_ = false;
    if (pos() != dragStartPos_) {
        if (onDragFinished_) {
            onDragFinished_(pos());
        }
        return;
    }
    if (reported && onDragReverted_) {
        onDragReverted_();
    }
    // Stationary click on an already-selected note requests editing.
    if (wasSelectedAtPress_ && isSelected() && onEditRequested_) {
        onEditRequested_();
    }
}

void NoteItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);

    // Normal: no surface. Hover: faint fill. Selected: accent outline, with the
    // hover fill kept underneath while hovered.
    if (isSelected()) {
        painter->setPen(QPen(selectedBorderColor(), 1.5));
        painter->setBrush(hovered_ ? QBrush(hoverFillColor()) : QBrush(Qt::NoBrush));
        painter->drawRoundedRect(rect_.adjusted(0.75, 0.75, -0.75, -0.75), kCornerRadius, kCornerRadius);
    } else if (hovered_) {
        painter->setPen(QPen(hoverBorderColor(), 1.0));
        painter->setBrush(QBrush(hoverFillColor()));
        painter->drawRoundedRect(rect_.adjusted(0.5, 0.5, -0.5, -0.5), kCornerRadius, kCornerRadius);
    }

    // Top-right fold; an element color tints it at low alpha.
    const QPolygonF fold({QPointF(rect_.right() - kFoldSize, rect_.top()), QPointF(rect_.right(), rect_.top()),
                           QPointF(rect_.right(), rect_.top() + kFoldSize)});
    painter->setPen(Qt::NoPen);
    QColor foldTint = foldColor();
    if (color_ != ElementColor::Default) {
        foldTint = elementAccent(color_);
        foldTint.setAlpha(90);
    }
    painter->setBrush(QBrush(foldTint));
    painter->drawPolygon(fold);

    const QRectF textRect = rect_.adjusted(kTextPadding, kTextPadding, -kTextPadding, -kTextPadding);
    if (text_.isEmpty()) {
        painter->setFont(noteFont());
        painter->setPen(placeholderColor());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, displayText(text_));
        return;
    }
    // The element color (or the resting gray) rides the palette's Text role,
    // coloring every run the markdown gave no explicit color.
    painter->save();
    painter->translate(textRect.topLeft());
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text,
                          color_ != ElementColor::Default ? elementAccent(color_) : textColor());
    doc_.documentLayout()->draw(painter, ctx);
    painter->restore();
}

}  // namespace app
