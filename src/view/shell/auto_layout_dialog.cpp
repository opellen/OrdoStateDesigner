#include "view/shell/auto_layout_dialog.h"

#include <algorithm>

#include <QChar>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QLabel>
#include <QMargins>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>

#include "constants/design_tokens.h"
#include "infra/settings_registry.h"
#include "infra/settings_store.h"

namespace app {

namespace {

// Wide enough for the label column plus the widest direction entry, so the
// combo never elides it.
constexpr int kMinWidth = 320;
// The canvas grid step: whole-grid gaps keep every laid-out box on the grid.
constexpr int kGapStep = 24;

const QString kDirectionKey = QStringLiteral("autoLayout.direction");
const QString kLayerGapKey = QStringLiteral("autoLayout.layerGap");
const QString kNodeGapKey = QStringLiteral("autoLayout.nodeGap");

// Renders a design token as a QSS pixel length.
QString px(int value) { return QString::number(value) + QStringLiteral("px"); }

// A gap setting's range, read from its registry definition so the spin box,
// the clamp in loadOptions and the Settings view share one source.
struct GapRange {
    int low = 0;
    int high = 0;
};

GapRange gapRange(const QString& key, qreal fallback) {
    const SettingDefinition* def = SettingsRegistry::instance().find(key);
    if (def == nullptr) {
        return {qRound(fallback), qRound(fallback)};
    }
    return {qRound(def->minimum), qRound(def->maximum)};
}

qreal loadGap(const SettingsStore& store, const QString& key, qreal fallback) {
    const QJsonValue value = store.get(key);
    if (!value.isDouble()) {
        return fallback;
    }
    const GapRange range = gapRange(key, fallback);
    return std::clamp(value.toDouble(), static_cast<qreal>(range.low), static_cast<qreal>(range.high));
}

QSpinBox* makeGapSpin(const QString& key, qreal value, QWidget* parent) {
    auto* spin = new QSpinBox(parent);
    const GapRange range = gapRange(key, value);
    spin->setRange(range.low, range.high);
    spin->setSingleStep(kGapStep);
    spin->setSuffix(QStringLiteral(" px"));
    spin->setValue(qRound(value));
    return spin;
}

}  // namespace

AutoLayoutDialog::AutoLayoutDialog(const AutoLayoutOptions& initial, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Auto Layout"));
    setModal(true);
    setMinimumWidth(kMinWidth);

    setStyleSheet(design::resolveRoles(QStringLiteral("QDialog {"
                                                      "  background-color: {surface-1};"
                                                      "  color: {text-primary};"
                                                      "}"
                                                      "QComboBox, QSpinBox {"
                                                      "  background-color: {surface-2};"
                                                      "  border: %1 solid {outline};"
                                                      "  border-radius: %2;"
                                                      "  padding: %3 %4;"
                                                      "  color: {text-primary};"
                                                      "}"
                                                      "QComboBox:focus, QSpinBox:focus {"
                                                      "  border: %1 solid {outline-focus};"
                                                      "}"))
                      .arg(px(design::kHairline))
                      .arg(px(design::kRadius))
                      .arg(px(design::kSpace1))
                      .arg(px(design::kSpace2)));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(design::kSpace4, design::kSpace4, design::kSpace4, design::kSpace4);
    rootLayout->setSpacing(design::kSpace4);

    auto* form = new QFormLayout();
    form->setContentsMargins(QMargins());
    form->setHorizontalSpacing(design::kSpace3);
    form->setVerticalSpacing(design::kSpace2);

    // Arrows by code point, never as literal characters in the source.
    directionCombo_ = new QComboBox(this);
    const struct {
        LayoutDirection direction;
        const char* text;
        char16_t arrow;
    } directionItems[] = {
        {LayoutDirection::LeftToRight, "Left to Right ", 0x2192},
        {LayoutDirection::RightToLeft, "Right to Left ", 0x2190},
        {LayoutDirection::TopToBottom, "Top to Bottom ", 0x2193},
        {LayoutDirection::BottomToTop, "Bottom to Top ", 0x2191},
    };
    for (const auto& item : directionItems) {
        directionCombo_->addItem(QString::fromLatin1(item.text) + QChar(item.arrow),
                                 layoutDirectionName(item.direction));
    }
    directionCombo_->setCurrentIndex(directionCombo_->findData(layoutDirectionName(initial.direction)));

    layerGapSpin_ = makeGapSpin(kLayerGapKey, initial.layerGap, this);
    nodeGapSpin_ = makeGapSpin(kNodeGapKey, initial.nodeGap, this);
    layerGapSpin_->setToolTip(QStringLiteral("Minimum distance between two ranks of states"));
    nodeGapSpin_->setToolTip(QStringLiteral("Distance between two states of the same rank"));

    const auto addRow = [this, form](const QString& text, QWidget* field) {
        auto* label = new QLabel(text, this);
        label->setStyleSheet(design::kTypeLabel);
        form->addRow(label, field);
    };
    addRow(QStringLiteral("Direction"), directionCombo_);
    addRow(QStringLiteral("Layer gap"), layerGapSpin_);
    addRow(QStringLiteral("Node gap"), nodeGapSpin_);
    rootLayout->addLayout(form);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(design::kSpace3);
    buttonLayout->addStretch(1);

    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    cancelButton->setStyleSheet(design::kSecondaryButtonStyle);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton);

    // The one Primary button on this screen.
    applyButton_ = new QPushButton(QStringLiteral("Apply"), this);
    applyButton_->setDefault(true);
    applyButton_->setStyleSheet(design::kPrimaryButtonStyle);
    connect(applyButton_, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(applyButton_);

    rootLayout->addLayout(buttonLayout);
}

AutoLayoutOptions AutoLayoutDialog::options() const {
    AutoLayoutOptions options;
    if (const std::optional<LayoutDirection> direction =
            layoutDirectionFromName(directionCombo_->currentData().toString())) {
        options.direction = *direction;
    }
    options.layerGap = layerGapSpin_->value();
    options.nodeGap = nodeGapSpin_->value();
    return options;
}

AutoLayoutOptions AutoLayoutDialog::loadOptions(const SettingsStore* store) {
    AutoLayoutOptions options;
    if (store == nullptr) {
        return options;
    }
    if (const std::optional<LayoutDirection> direction =
            layoutDirectionFromName(store->get(kDirectionKey).toString())) {
        options.direction = *direction;
    }
    options.layerGap = loadGap(*store, kLayerGapKey, options.layerGap);
    options.nodeGap = loadGap(*store, kNodeGapKey, options.nodeGap);
    return options;
}

void AutoLayoutDialog::storeOptions(SettingsStore* store, const AutoLayoutOptions& options) {
    if (store == nullptr) {
        return;
    }
    store->set(kDirectionKey, layoutDirectionName(options.direction), StoreScope::User);
    store->set(kLayerGapKey, qRound(options.layerGap), StoreScope::User);
    store->set(kNodeGapKey, qRound(options.nodeGap), StoreScope::User);
}

}  // namespace app
