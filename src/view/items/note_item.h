#pragma once

#include <functional>

#include <QGraphicsItem>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTextDocument>
#include <QtGlobal>

#include "model/machine.h"  // ElementColor

class QGraphicsSceneHoverEvent;
class QGraphicsSceneMouseEvent;

namespace app {

// Renders one Note: bare text at rest (an empty note shows a "Type a note"
// placeholder), a faint surface on hover, an accent outline when selected, and a
// small fold in the top-right corner. Content-sized: markdown laid out in an owned
// QTextDocument, word-wrapped at a max width. Drag gestures follow StateItem.
// pos() is the box's top-left corner; boundingRect() is local, origin (0,0).
class NoteItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 6 };
    int type() const override { return Type; }

    NoteItem(quint64 id, QString text);

    quint64 id() const { return id_; }

    void setText(const QString& text);
    const QString& text() const { return text_; }
    // Tints the note's text and fold; hover/selected chrome is unchanged.
    void setColor(ElementColor color);

    // This item's rect in scene coordinates.
    QRectF sceneRect() const;

    // Same callback shape as StateItem's drag callbacks.
    void setOnMoved(std::function<void()> cb);
    void setOnDragStarted(std::function<void()> cb);
    void setOnDragFinished(std::function<void(QPointF pos)> cb);
    void setOnDragReverted(std::function<void()> cb);
    // Stationary left click on an already-selected note.
    void setOnEditRequested(std::function<void()> cb);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    void recomputeGeometry();  // content-sized box from the wrapped text

    quint64 id_;
    QString text_;
    ElementColor color_ = ElementColor::Default;
    QRectF rect_;  // local; (0,0) at top-left; content-sized
    QTextDocument doc_;  // rendered-markdown layout; rebuilt by recomputeGeometry

    bool hovered_ = false;
    bool wasSelectedAtPress_ = false;  // selection state sampled at press, for the edit gesture
    bool leftPressed_ = false;         // between press and release: what makes a position change a DRAG
    bool dragReported_ = false;        // onDragStarted_ already fired for this gesture
    QPointF dragStartPos_;

    std::function<void()> onMoved_;
    std::function<void()> onDragStarted_;
    std::function<void(QPointF)> onDragFinished_;
    std::function<void()> onDragReverted_;
    std::function<void()> onEditRequested_;
};

}  // namespace app
