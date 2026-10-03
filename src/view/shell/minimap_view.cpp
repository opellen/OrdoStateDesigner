#include "view/shell/minimap_view.h"

#include <QColor>
#include <QFrame>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>

#include "constants/design_tokens.h"

namespace app {

namespace {

constexpr int kMinimapWidth = 180;
constexpr int kMinimapHeight = 120;
constexpr qreal kFitMargin = 40.0;  // matches CanvasView::centerAndFit's margin
constexpr int kRefitCoalesceMs = 60;  // debounce window for scene->changed() bursts

// Body uses the surface-overlay role, the visible-rect overlay the accent role.
// The border is set once via QSS in the constructor.
QColor minimapBackground() { return design::color(design::kSurfaceOverlay); }  // near-opaque surface-overlay
QColor visibleRectAccent() { return design::color(design::kAccentInteractive); }

}  // namespace

MinimapView::MinimapView(QWidget* parent) : QGraphicsView(parent) {
    // Read-only navigator: the only interaction is click/drag-to-jump, handled
    // in mousePressEvent/mouseMoveEvent.
    setInteractive(false);
    setDragMode(QGraphicsView::NoDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::NoFocus);
    setRenderHint(QPainter::Antialiasing);
    // NoFrame + QSS border: once QSS targets the widget its box model paints the
    // frame anyway. drawBackground() paints the content fill, which QSS
    // `background` on a QAbstractScrollArea does not reliably reach.
    setFrameShape(QFrame::NoFrame);
    setStyleSheet(design::resolveRoles(QStringLiteral("QGraphicsView { border: 1px solid {outline}; }")));
    setFixedSize(kMinimapWidth, kMinimapHeight);

    refitTimer_ = new QTimer(this);
    refitTimer_->setSingleShot(true);
    refitTimer_->setInterval(kRefitCoalesceMs);
    connect(refitTimer_, &QTimer::timeout, this, &MinimapView::refit);
}

void MinimapView::bindScene(QGraphicsScene* scene) {
    QObject::disconnect(sceneChangedConnection_);
    setScene(scene);  // QGraphicsView::setScene (not shadowed)
    if (scene != nullptr) {
        sceneChangedConnection_ =
            connect(scene, &QGraphicsScene::changed, this, [this](const QList<QRectF>&) { scheduleRefit(); });
    }
    update();
    scheduleRefit();
}

void MinimapView::setSourceView(QGraphicsView* view) {
    QObject::disconnect(sourceHConnection_);
    QObject::disconnect(sourceVConnection_);
    sourceView_ = view;
    if (sourceView_ != nullptr) {
        // Pan and most zoom move the scrollbars: update() keeps the overlay
        // current; scheduleRefit() is the throttled re-fit for extent changes.
        sourceHConnection_ = connect(sourceView_->horizontalScrollBar(), &QAbstractSlider::valueChanged, this,
                                      [this](int) {
                                          update();
                                          scheduleRefit();
                                      });
        sourceVConnection_ = connect(sourceView_->verticalScrollBar(), &QAbstractSlider::valueChanged, this,
                                      [this](int) {
                                          update();
                                          scheduleRefit();
                                      });
    }
}

void MinimapView::refresh() {
    update();
    scheduleRefit();
}

void MinimapView::debugMousePress(QPointF scenePos) {
    const QPoint viewportPos = mapFromScene(scenePos);
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(viewportPos), mapToGlobal(viewportPos), Qt::LeftButton,
                       Qt::LeftButton, Qt::NoModifier);
    mousePressEvent(&event);
}

void MinimapView::drawBackground(QPainter* painter, const QRectF& rect) {
    // Flat panel fill, no grid: a miniature grid would be noise at this scale.
    painter->fillRect(rect, minimapBackground());
}

void MinimapView::drawForeground(QPainter* painter, const QRectF& rect) {
    QGraphicsView::drawForeground(painter, rect);
    const QRectF visible = sourceVisibleSceneRect();
    if (!visible.isValid() || visible.isEmpty()) {
        return;
    }
    painter->save();
    QPen pen(visibleRectAccent());
    pen.setCosmetic(true);  // stays a crisp 1px line at any minimap zoom
    pen.setWidthF(1.0);
    painter->setPen(pen);
    QColor fill = visibleRectAccent();
    fill.setAlpha(40);
    painter->setBrush(fill);
    painter->drawRect(visible);
    painter->restore();
}

void MinimapView::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    if (pendingFit_) {
        scheduleRefit();
    }
}

void MinimapView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        navigateTo(event->position().toPoint());
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void MinimapView::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons().testFlag(Qt::LeftButton)) {
        navigateTo(event->position().toPoint());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void MinimapView::scheduleRefit() { refitTimer_->start(); }  // restarting before it fires coalesces the burst

void MinimapView::refit() {
    if (scene() == nullptr) {
        return;
    }
    QRectF bounds = scene()->itemsBoundingRect().united(sourceVisibleSceneRect());
    if (bounds.isEmpty()) {
        return;
    }
    if (viewport() == nullptr || viewport()->size().isEmpty()) {
        pendingFit_ = true;  // retried on the next real resizeEvent
        return;
    }
    fitInView(bounds.adjusted(-kFitMargin, -kFitMargin, kFitMargin, kFitMargin), Qt::KeepAspectRatio);
    pendingFit_ = false;
    update();
}

QRectF MinimapView::sourceVisibleSceneRect() const {
    if (sourceView_ == nullptr || sourceView_->viewport() == nullptr) {
        return QRectF();
    }
    return sourceView_->mapToScene(sourceView_->viewport()->rect()).boundingRect();
}

void MinimapView::navigateTo(const QPoint& viewportPos) {
    if (onNavigate) {
        onNavigate(mapToScene(viewportPos));
    }
}

}  // namespace app
