#include "view/shell/icons.h"

#include <QColor>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include "constants/design_tokens.h"

namespace app::icons {

namespace {

// Tint colors come from the design tokens; QColor has no constexpr
// string-parsing constructor, hence non-constexpr locals.
const QColor kGlyphNormal(QString::fromLatin1(design::kIconNormal));
const QColor kGlyphActive(QString::fromLatin1(design::kIconActive));
const QColor kGlyphDisabled(QString::fromLatin1(design::kIconDisabled));

// Renders the vendored SVG silhouette for `slug`, tinted to `tint`.
QPixmap renderSvgPixmap(const QString& slug, const QColor& tint, int size) {
    // One renderer per slug, created on first use and kept for the process
    // lifetime (QSvgRenderer parses up front and isn't copyable).
    static QHash<QString, QSvgRenderer*> renderers;
    QSvgRenderer* renderer = renderers.value(slug, nullptr);
    if (!renderer) {
        renderer = new QSvgRenderer(QStringLiteral(":/icons/%1.svg").arg(slug));
        renderers.insert(slug, renderer);
    }

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer->render(&painter, QRectF(0, 0, size, size));
    // Every asset is a single charcoal (#1F2933) silhouette; SourceIn
    // replaces its opaque pixels with the caller's tint while keeping the
    // silhouette's own alpha (including its evenodd holes) as the mask.
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), tint);
    return pixmap;
}

// IconId -> vendored slug. Run maps to "play"; there is no run.svg.
const char* slugFor(IconId id) {
    switch (id) {
        case IconId::Design:      return "design";
        case IconId::Simulate:    return "simulate";
        case IconId::Run:         return "play";
        case IconId::Pause:       return "pause";
        case IconId::Reset:       return "reset";
        case IconId::Fit:         return "fit";
        case IconId::GenerateCpp: return "generate-cpp";
        case IconId::Hook:        return "hook";
        case IconId::Expr:        return "expr";
        case IconId::Assign:      return "assign";
        case IconId::Raise:       return "raise";
        case IconId::SendTo:      return "send-to";
        case IconId::SendParent:  return "send-parent";
    }
    return "";  // unreachable -- the switch above is exhaustive over IconId
}

// Six-state (Normal/Active/Disabled x Off/On) assembly at several raster
// sizes, so Qt can pick a crisp match for any icon size x devicePixelRatio.
QIcon assembleStates(const QString& slug) {
    QIcon result;
    for (int size : {16, 20, 24, 32, 48, 64}) {
        result.addPixmap(renderSvgPixmap(slug, kGlyphNormal, size), QIcon::Normal, QIcon::Off);
        result.addPixmap(renderSvgPixmap(slug, kGlyphNormal, size), QIcon::Active, QIcon::Off);
        result.addPixmap(renderSvgPixmap(slug, kGlyphActive, size), QIcon::Normal, QIcon::On);
        result.addPixmap(renderSvgPixmap(slug, kGlyphActive, size), QIcon::Active, QIcon::On);
        result.addPixmap(renderSvgPixmap(slug, kGlyphDisabled, size), QIcon::Disabled, QIcon::Off);
        result.addPixmap(renderSvgPixmap(slug, kGlyphDisabled, size), QIcon::Disabled, QIcon::On);
    }
    return result;
}

}  // namespace

QIcon statusDot(bool active) {
    QIcon result;
    const QColor color = active ? QColor(QString::fromLatin1(design::kStatusLive))
                                 : QColor(QString::fromLatin1(design::kStatusIdle));
    for (int size : {10, 12, 16, 20, 24}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const qreal margin = size * 0.28;
        painter.drawEllipse(QRectF(margin, margin, size - 2 * margin, size - 2 * margin));
        result.addPixmap(pixmap);
    }
    return result;
}

QIcon closeGlyph() {
    QIcon result;
    for (int size : {12, 16, 20, 24}) {
        result.addPixmap(renderSvgPixmap(QStringLiteral("remove"), kGlyphNormal, size));
    }
    return result;
}

QIcon icon(IconId id) {
    return assembleStates(QString::fromLatin1(slugFor(id)));
}

QIcon asset(const char* slug) {
    return assembleStates(QString::fromLatin1(slug));
}

QPixmap assetPixmap(const char* slug, int size, const QColor& tint, qreal devicePixelRatio) {
    QPixmap pixmap = renderSvgPixmap(QString::fromLatin1(slug), tint, qRound(size * devicePixelRatio));
    pixmap.setDevicePixelRatio(devicePixelRatio);
    return pixmap;
}

// Unlike the glyphs above, the brand mark keeps its own colors: rendered
// untinted at every size Windows asks a window icon for.
QIcon appIcon() {
    QIcon result;
    QSvgRenderer renderer(QStringLiteral(":/brand/logo-mark.svg"));
    for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(0, 0, size, size));
        result.addPixmap(pixmap);
    }
    return result;
}

}  // namespace app::icons
