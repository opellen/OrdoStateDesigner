#pragma once

#include <QDialog>

#include "view/geometry/auto_layout.h"

class QComboBox;
class QPushButton;
class QSpinBox;

namespace app {

class SettingsStore;

// The Auto Layout... dialog: direction and the two spacing gaps, Apply or
// Cancel. It only collects options; on accept the caller runs
// DocumentSession::runAutoLayout with options() and stores them with
// storeOptions(). exec() for the real UI, show() + the debug accessors for probes.
class AutoLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AutoLayoutDialog(const AutoLayoutOptions& initial, QWidget* parent = nullptr);

    // What the fields say now: the combo's direction and the two gaps.
    AutoLayoutOptions options() const;

    // The autoLayout.* settings as options. A missing or unusable value falls
    // back to the engine default; an out-of-range gap is clamped. Widget-free.
    static AutoLayoutOptions loadOptions(const SettingsStore* store);
    // Writes the three autoLayout.* keys to `store`'s User scope. No-op on a
    // null store.
    static void storeOptions(SettingsStore* store, const AutoLayoutOptions& options);

    // Probe access (the dialog's own widgets; valid for its lifetime).
    QComboBox* debugDirectionCombo() const { return directionCombo_; }
    QSpinBox* debugLayerGapSpin() const { return layerGapSpin_; }
    QSpinBox* debugNodeGapSpin() const { return nodeGapSpin_; }
    QPushButton* debugApplyButton() const { return applyButton_; }

private:
    QComboBox* directionCombo_ = nullptr;
    QSpinBox* layerGapSpin_ = nullptr;
    QSpinBox* nodeGapSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
};

}  // namespace app
