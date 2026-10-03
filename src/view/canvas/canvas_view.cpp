#include "view/canvas/canvas_view.h"

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QContextMenuEvent>
#include <QGraphicsItem>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QWheelEvent>

#include "constants/design_tokens.h"
#include "infra/settings_store.h"

namespace app {

namespace {

constexpr qreal kMinZoom = 0.15;
constexpr qreal kMaxZoom = 6.0;
constexpr qreal kZoomStep = 1.15;
constexpr qreal kGridStep = 24.0;  // default grid pitch, scene px
constexpr qreal kFitMargin = 40.0;  // breathing room around content for centerAndFit

QColor canvasBackground() { return design::color(design::kCanvasBackground); }
QColor gridLine() { return design::color(design::kCanvasGrid); }

}  // namespace

CanvasView::CanvasView(QWidget* parent) : QGraphicsView(parent) {
    setRenderHint(QPainter::Antialiasing);

    if (auto* store = SettingsStore::activeStore()) {
        connect(store, &SettingsStore::settingChanged, this, [this](const QString& key, auto...) {
            if (key == QStringLiteral("canvas.gridStep")) {
                if (viewport() != nullptr) {
                    viewport()->update();
                }
            }
        });
    }
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    // Pan scrolls by scrollbar value, which silently no-ops without range, so
    // give the view a huge scene rect and hide the bars.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSceneRect(QRectF(-50000.0, -50000.0, 100000.0, 100000.0));
    // QGraphicsView does not take keyboard focus by default.
    setFocusPolicy(Qt::StrongFocus);
}

void CanvasView::centerAndFit(const QRectF& contentRect) {
    if (contentRect.isEmpty() || viewport() == nullptr) {
        return;
    }
    const QRectF padded = contentRect.adjusted(-kFitMargin, -kFitMargin, kFitMargin, kFitMargin);
    const QSize viewportSize = viewport()->size();
    if (viewportSize.isEmpty() || padded.width() <= 0.0 || padded.height() <= 0.0) {
        return;
    }
    qreal fitScale = std::min(viewportSize.width() / padded.width(), viewportSize.height() / padded.height());
    fitScale = std::clamp(fitScale, kMinZoom, 1.0);  // never upscale past 1.0

    resetTransform();
    scale(fitScale, fitScale);
    zoomFactor_ = fitScale;  // keep wheelEvent's clamp baseline in sync
    centerOn(padded.center());
}

void CanvasView::revealRect(const QRectF& contentRect) {
    if (contentRect.isEmpty()) {
        return;
    }
    // ensureVisible's margins are in viewport pixels; kFitMargin is scene
    // units, so scale it by the current zoom to keep the same breathing room.
    const int margin = qRound(kFitMargin * transform().m11());
    ensureVisible(contentRect, margin, margin);
}

void CanvasView::resetView(const QRectF& contentRect) {
    resetTransform();
    zoomFactor_ = 1.0;
    centerOn(contentRect.isEmpty() ? QPointF(0.0, 0.0) : contentRect.center());
}

void CanvasView::drawBackground(QPainter* painter, const QRectF& rect) {
    painter->fillRect(rect, canvasBackground());

    QPen pen(gridLine());
    pen.setCosmetic(true);  // stays a crisp 1px line at any zoom level
    painter->setPen(pen);

    auto* store = SettingsStore::activeStore();
    const qreal step = store ? store->get(QStringLiteral("canvas.gridStep"), kGridStep).toDouble() : kGridStep;
    const qreal left = std::floor(rect.left() / step) * step;
    const qreal top = std::floor(rect.top() / step) * step;
    for (qreal x = left; x < rect.right(); x += step) {
        painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
    }
    for (qreal y = top; y < rect.bottom(); y += step) {
        painter->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }
}

void CanvasView::wheelEvent(QWheelEvent* event) {
    // AnchorUnderMouse makes scale() pivot on the cursor.
    const qreal factor = event->angleDelta().y() > 0 ? kZoomStep : 1.0 / kZoomStep;
    const qreal nextZoom = zoomFactor_ * factor;
    if (nextZoom < kMinZoom || nextZoom > kMaxZoom) {
        event->accept();
        return;
    }
    zoomFactor_ = nextZoom;
    scale(factor, factor);
    event->accept();
}

void CanvasView::mousePressEvent(QMouseEvent* event) {
    lastMouseScenePos_ = mapToScene(event->position().toPoint());
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        panStartViewportPos_ = event->position().toPoint();
        panStartHScroll_ = horizontalScrollBar()->value();
        panStartVScroll_ = verticalScrollBar()->value();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && itemAt(event->position().toPoint()) == nullptr) {
        // Empty canvas only: an item claims its own press anyway, and a band
        // opened here would leave that item's drag with no session to commit.
        setDragMode(QGraphicsView::RubberBandDrag);
        emit rubberBandStarted();
    }
    QGraphicsView::mousePressEvent(event);
}

void CanvasView::mouseMoveEvent(QMouseEvent* event) {
    lastMouseScenePos_ = mapToScene(event->position().toPoint());
    if (panning_) {
        const QPoint delta = event->position().toPoint() - panStartViewportPos_;
        horizontalScrollBar()->setValue(panStartHScroll_ - delta.x());
        verticalScrollBar()->setValue(panStartVScroll_ - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void CanvasView::mouseReleaseEvent(QMouseEvent* event) {
    lastMouseScenePos_ = mapToScene(event->position().toPoint());
    if (panning_ && event->button() == Qt::MiddleButton) {
        panning_ = false;
        unsetCursor();
        event->accept();
        return;
    }
    // RubberBandDrag is set only between the empty-canvas press and this
    // release; read it before the base call applies the band selection.
    const bool wasBanding = dragMode() == QGraphicsView::RubberBandDrag;
    QGraphicsView::mouseReleaseEvent(event);
    if (wasBanding && event->button() == Qt::LeftButton) {
        setDragMode(QGraphicsView::NoDrag);
        emit rubberBandFinished();
    }
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QPoint viewportPos = event->position().toPoint();
        // Only an empty-canvas double-click is reported; items handle their own.
        if (itemAt(viewportPos) == nullptr) {
            emit emptyCanvasDoubleClicked(mapToScene(viewportPos));
            event->accept();
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void CanvasView::keyPressEvent(QKeyEvent* event) {
    // While the inline editor (the only focusable scene item) has focus, every
    // key is its own: Backspace edits text instead of deleting the node.
    if (scene() != nullptr && scene()->focusItem() != nullptr) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (dragMode() == QGraphicsView::RubberBandDrag) {
            // Cannot undo what the band already selected; this only keeps a
            // still-held button from starting a fresh band on the next move.
            setDragMode(QGraphicsView::NoDrag);
        }
        emit escapePressed();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        emit spacePressed();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F && event->modifiers() == Qt::NoModifier && !event->isAutoRepeat()) {
        emit zoomToSelectionRequested();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        emit deleteRequested();
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void CanvasView::contextMenuEvent(QContextMenuEvent* event) {
    emit contextMenuRequested(mapToScene(event->pos()), event->globalPos());
    event->accept();
}

void CanvasView::debugMousePress(QPointF scenePos, Qt::KeyboardModifiers modifiers) {
    lastMouseScenePos_ = scenePos;
    const QPoint viewportPos = mapFromScene(scenePos);
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(viewportPos), mapToGlobal(viewportPos), Qt::LeftButton,
                       Qt::LeftButton, modifiers);
    mousePressEvent(&event);
}

void CanvasView::debugMouseMove(QPointF scenePos) {
    lastMouseScenePos_ = scenePos;
    const QPoint viewportPos = mapFromScene(scenePos);
    QMouseEvent event(QEvent::MouseMove, QPointF(viewportPos), mapToGlobal(viewportPos), Qt::NoButton,
                       Qt::LeftButton, Qt::NoModifier);
    mouseMoveEvent(&event);
}

void CanvasView::debugMouseRelease(QPointF scenePos) {
    lastMouseScenePos_ = scenePos;
    const QPoint viewportPos = mapFromScene(scenePos);
    QMouseEvent event(QEvent::MouseButtonRelease, QPointF(viewportPos), mapToGlobal(viewportPos), Qt::LeftButton,
                       Qt::NoButton, Qt::NoModifier);
    mouseReleaseEvent(&event);
}

}  // namespace app
