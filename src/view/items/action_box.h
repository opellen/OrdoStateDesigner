#pragma once

#include <functional>
#include <vector>

#include <QFont>
#include <QGraphicsItem>
#include <QPointF>
#include <QRectF>
#include <QString>

class QGraphicsSceneHoverEvent;
class QGraphicsSceneMouseEvent;

namespace app {

// Forward declaration: SelectionKind lives in canvas_presenter.h, which includes
// this header.
enum class SelectionKind;

// One verb a segment of the box can fire; CanvasPresenter maps each to an
// existing intent. Variants:
//   Frame:      AddMachineEvent, AddMachineSelf, AddNote, FrameMore (icon-only)
//   State:      AddTransition, AddSelfTransition, StateMore
//   Transition: SetGuard, SetAction, ReverseDirection, TransitionMore
//   Multi:      ZoomToSelection, MultiColor, DeleteSelection
enum class ActionBoxVerb {
    ZoomToSelection,
    MultiColor,
    DeleteSelection,
    AddMachineEvent,
    AddMachineSelf,
    AddState,
    AddNote,
    FrameMore,
    AddTransition,
    AddSelfTransition,
    StateMore,
    SetGuard,
    SetAction,
    ReverseDirection,
    TransitionMore,
};

// Contextual floating action box: a one-line capsule of icon/text segments
// anchored just below the current selection's bounding rect.
// Pure view: CanvasPresenter decides when it shows and what each segment does;
// showFor()/hideBox() are the whole lifecycle. One item paints all segments.
// One instance per presenter, hidden (never destroyed) between appearances;
// the presenter resets its pointer after scene_->clear() deletes it.
class ActionBoxItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 7 };
    int type() const override { return Type; }

    ActionBoxItem();

    // Rebuilds the segment row for `kind` and shows the box centered just below
    // `anchorSceneRect`. Kinds without a variant hide the box instead.
    // `reverseEnabled` gates the Transition variant's reverse segment (a
    // targetless transition has nothing to reverse).
    void showFor(SelectionKind kind, const QRectF& anchorSceneRect, bool reverseEnabled);
    void hideBox();

    // Fired on a left-click on an enabled segment. Does not hide the box; the
    // presenter does that.
    void setOnVerb(std::function<void(ActionBoxVerb)> cb);

    // Probe hooks, all in on-screen order: segment labels (empty for icon
    // segments), icon slugs (empty for word segments), hover tooltips.
    std::vector<QString> debugSegmentLabels() const;
    std::vector<QString> debugSegmentIconSlugs() const;
    std::vector<QString> debugSegmentTooltips() const;
    // The pixel ratio the last paint rendered its icon pixmaps at (device
    // pixel ratio x the view's zoom); 0 before the first paint.
    qreal debugLastIconRenderRatio() const { return lastIconRenderRatio_; }

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

private:
    struct Segment {
        ActionBoxVerb verb;
        QString label;  // word segments' visible text; empty for an icon segment
        QString tooltip;  // hover tooltip; the verb name for icon-only segments
        bool enabled = true;
        QRectF rect;  // local coords; assigned by layout()
        // Vendored SVG slug resolved via icons::assetPixmap() at paint time;
        // nullptr for a word segment. Kept last so word-segment aggregate
        // inits can omit it.
        const char* iconSlug = nullptr;
    };

    // Recomputes every segment rect and capsuleRect_; called by showFor() only,
    // since content is fixed until hideBox().
    void layout();

    QFont font_;
    QRectF capsuleRect_;  // local; (0,0) at top-left, sized to fit segments_
    std::vector<Segment> segments_;
    int hoveredIndex_ = -1;
    qreal lastIconRenderRatio_ = 0.0;
    std::function<void(ActionBoxVerb)> onVerb_;
};

}  // namespace app
