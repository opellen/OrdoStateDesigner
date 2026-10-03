#pragma once

#include <functional>
#include <QFont>
#include <QGraphicsItem>
#include <QRectF>
#include <QString>
#include <QStringList>

class QGraphicsSceneMouseEvent;
class QGraphicsSceneHoverEvent;

namespace app {

// Floating in-canvas HUD showing the projected C++ code next to the selected
// state or transition.
class CodeLensItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 8 };
    int type() const override { return Type; }

    CodeLensItem();

    void showLens(const QString& title, const QString& code, const QRectF& anchorRect);
    void hideLens();

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

    void setOnCloseClicked(std::function<void()> cb);

    bool isLensVisible() const { return isVisible(); }
    QString debugTitle() const { return title_; }
    QString debugCode() const { return code_; }
    QRectF debugRect() const { return rect_; }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    void layoutLens(const QRectF& anchorRect);

    QString title_;
    QString code_;
    QStringList lines_;
    QRectF rect_;
    QRectF closeButtonRect_;
    bool closeHovered_ = false;
    QFont headerFont_;
    QFont codeFont_;
    std::function<void()> onCloseClicked_;
};

}  // namespace app
