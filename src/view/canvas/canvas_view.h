#pragma once

#include <QGraphicsView>
#include <QPoint>
#include <QPointF>
#include <QRectF>

class QWheelEvent;
class QMouseEvent;
class QKeyEvent;
class QContextMenuEvent;
class QGraphicsItem;

namespace app {

// Domain-blind canvas view: grid, wheel zoom, middle-button pan, and raw input
// signals for CanvasPresenter. Left drags belong to items, except that a press
// on empty canvas runs Qt's RubberBandDrag until release.
// z-bands: frame(-10) < containers(-5..-1 by depth) < edges(0) < notes(5) <
// nodes(10) < initial marker(11) < labels(20) < interaction overlay(100).
class CanvasView : public QGraphicsView {
    Q_OBJECT

public:
    explicit CanvasView(QWidget* parent = nullptr);

    // Centers `contentRect` (scene coordinates) with a margin, never zooming
    // past 1.0. Call after the view is shown; before that it is a no-op.
    void centerAndFit(const QRectF& contentRect);

    // Zooms back to 1:1 centered on `contentRect`.
    void resetView(const QRectF& contentRect);
    // Scrolls just enough to bring `contentRect` (plus the fit margin) into
    // view at the CURRENT zoom; no-op when it is already visible.
    void revealRect(const QRectF& contentRect);

    // Probe levers: synthesize a left-button press/move/release through this
    // view's own mouse handlers. `scenePos` is mapped to integer viewport
    // coordinates, so the round trip can land sub-pixel off the request.
    void debugMousePress(QPointF scenePos, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void debugMouseMove(QPointF scenePos);
    void debugMouseRelease(QPointF scenePos);
    QPointF lastMouseScenePos() const { return lastMouseScenePos_; }

signals:
    void emptyCanvasDoubleClicked(QPointF scenePos);
    void deleteRequested();
    void escapePressed();
    void spacePressed();
    // F with no modifiers; the presenter decides whether it may run.
    void zoomToSelectionRequested();
    void contextMenuRequested(QPointF scenePos, QPoint globalPos);
    // Bookends of an empty-canvas band drag; Qt applies the selection itself.
    void rubberBandStarted();
    void rubberBandFinished();

protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    // Current zoom, tracked so it can be clamped without decomposing the
    // transform. Every transform reset must update it.
    qreal zoomFactor_ = 1.0;

    // Middle-button pan moves the scrollbars directly instead of switching
    // drag mode, so left-button item interaction is never disturbed.
    bool panning_ = false;
    QPoint panStartViewportPos_;
    int panStartHScroll_ = 0;
    int panStartVScroll_ = 0;
    QPointF lastMouseScenePos_;
};

}  // namespace app
