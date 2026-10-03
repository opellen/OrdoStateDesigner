#pragma once

#include <functional>

#include <QGraphicsView>
#include <QPointF>
#include <QRectF>
#include <QtGlobal>

class QGraphicsScene;
class QMouseEvent;
class QPainter;
class QResizeEvent;
class QTimer;

namespace app {

// Small read-only corner navigator for one EditorView pane: a second
// QGraphicsView over the scene the pane's CanvasView renders. Domain-blind; it
// reports clicks as scene coordinates through onNavigate, which the owner wires
// to the canvas view's centerOn().
class MinimapView : public QGraphicsView {
    Q_OBJECT

public:
    explicit MinimapView(QWidget* parent = nullptr);

    // Shares `scene` with the pane's main CanvasView (nullptr while unbound).
    // Not named setScene(): the base method is non-virtual, so a same-named
    // override would be bypassed through a QGraphicsView*.
    void bindScene(QGraphicsScene* scene);

    // The main view this minimap navigates (non-owning). Read only, to derive
    // the visible-rect overlay and follow its pan/zoom.
    void setSourceView(QGraphicsView* view);

    // Re-fit request; throttled like every other trigger, so cheap to call often.
    void refresh();

    // Click/drag-to-jump target, in scene coordinates. A std::function, not a
    // signal: there is exactly one owner.
    std::function<void(QPointF)> onNavigate;

    // Probe lever: synthesizes a real press through mousePressEvent();
    // `scenePos` is mapped to viewport coordinates first.
    void debugMousePress(QPointF scenePos);

protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    // Restarts a short timer so a burst of triggers fires refit() once; a
    // canvas rebuild recreates every scene item, so changed() arrives in bursts.
    void scheduleRefit();
    void refit();

    // The main view's current visible scene rect; recomputed each call, never cached.
    QRectF sourceVisibleSceneRect() const;

    void navigateTo(const QPoint& viewportPos);

    QGraphicsView* sourceView_ = nullptr;  // weak, non-owning
    QTimer* refitTimer_ = nullptr;         // single-shot; owned via QObject parent (this)
    QMetaObject::Connection sceneChangedConnection_;
    QMetaObject::Connection sourceHConnection_;
    QMetaObject::Connection sourceVConnection_;

    // Set when refit() ran against an empty viewport (fitInView silently no-ops
    // before the viewport has a real size); retried on the next resizeEvent.
    bool pendingFit_ = false;
};

}  // namespace app
