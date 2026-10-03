#include "view/shell/settings_view.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleOptionSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include "constants/design_tokens.h"
#include "view/shell/icons.h"

namespace app {

namespace {

// One-screen sizes: no other screen shares these, so they stay local rather
// than becoming tokens.
constexpr int kCategoryListWidth = 180;    // the longest category name at the 12px ramp, plus selection bar
constexpr int kSearchFieldMinWidth = 280;  // keeps the example placeholder readable
constexpr int kTextFieldMinWidth = 200;    // paths and free-text values
constexpr int kEnumFieldMinWidth = 152;
constexpr int kIntFieldMinWidth = 96;
constexpr int kSpinButtonWidth = 20;       // SettingSpinBox's painted chevron slot
constexpr int kCheckIndicatorSize = 16;
constexpr int kEmptyStateTopMargin = 40;   // "no matching settings" sits clear of the header

QString px(int value) { return QStringLiteral("%1px").arg(value); }

// One sheet for the whole view: fields share a single skin, cards and the
// category list share the state layers, and all sizes come from tokens.
QString settingsStyleSheet() {
    return design::resolveRoles(QStringLiteral(
                                    "#settingsView { background: {surface-0}; }"
                                    "#settingsCards, #settingsScroll { background: transparent; border: none; }"
                                    "#settingsSplitter::handle { background: {outline}; width: %1; }"
                                    "#settingItemCard {"
                                    "  background: {surface-2};"
                                    "  border: %1 solid {outline};"
                                    "  border-radius: %2;"
                                    "}"
                                    "#settingItemCard:hover { border-color: {outline-strong}; }"
                                    "QLineEdit, QComboBox, QSpinBox {"
                                    "  background: {surface-2};"
                                    "  color: {text-primary};"
                                    "  border: %1 solid {outline};"
                                    "  border-radius: %2;"
                                    "  padding: %3 %4;"
                                    "  font-size: %9;"
                                    "  font-weight: 400;"
                                    "}"
                                    "QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: {outline-focus}; }"
                                    "QComboBox QAbstractItemView {"
                                    "  background: {surface-2};"
                                    "  color: {text-primary};"
                                    "  selection-background-color: {surface-hover};"
                                    "}"
                                    "QSpinBox { padding-right: %5; }"
                                    "QSpinBox::up-button, QSpinBox::down-button {"
                                    "  subcontrol-origin: border;"
                                    "  width: %6;"
                                    "  background: transparent;"
                                    "  border: none;"
                                    "}"
                                    "QSpinBox::up-button { subcontrol-position: top right; }"
                                    "QSpinBox::down-button { subcontrol-position: bottom right; }"
                                    "QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: {surface-hover}; }"
                                    "QCheckBox { color: {text-primary}; font-size: %9; font-weight: 400; spacing: %4; }"
                                    "QCheckBox::indicator {"
                                    "  width: %7;"
                                    "  height: %7;"
                                    "  border-radius: %2;"
                                    "  border: %1 solid {outline};"
                                    "  background: {surface-2};"
                                    "}"
                                    "QCheckBox::indicator:checked { background: {accent-brand}; border-color: {accent-interactive}; }"
                                    "#settingsCategories {"
                                    "  background: {surface-2};"
                                    "  border: %1 solid {outline};"
                                    "  border-radius: %2;"
                                    "  color: {text-secondary};"
                                    "  font-size: %9;"
                                    "  outline: none;"
                                    "  padding: %3;"
                                    "}"
                                    "#settingsCategories::item {"
                                    "  padding: %3 %4;"
                                    "  border-radius: %2;"
                                    "  border-left: %8 solid transparent;"
                                    "}"
                                    "#settingsCategories::item:hover:!selected { background: {surface-hover}; color: {text-primary}; }"
                                    "#settingsCategories::item:selected {"
                                    "  background: {surface-hover};"
                                    "  color: {text-primary};"
                                    "  border-left: %8 solid {accent-interactive};"
                                    "}"))
        .arg(px(design::kHairline))
        .arg(px(design::kRadius))
        .arg(px(design::kSpace1))
        .arg(px(design::kSpace2))
        .arg(px(kSpinButtonWidth + design::kSpace1))
        .arg(px(kSpinButtonWidth))
        .arg(px(kCheckIndicatorSize))
        .arg(px(design::kSelectionBarWidth))
        .arg(px(design::kTypeSizePx));
}

// A ramp fragment recolored: QSS declarations are last-wins within one rule,
// so appending a color replaces only the color.
QString recolored(const QString& typeRamp, const char* role) {
    return typeRamp + design::resolveRoles(QStringLiteral("color: {%1};").arg(QLatin1String(role)));
}

}  // namespace

// Custom spinbox with sharp, resolution-independent QPainter chevron arrows
class SettingSpinBox : public QSpinBox {
public:
    explicit SettingSpinBox(QWidget* parent = nullptr) : QSpinBox(parent) {}

protected:
    void paintEvent(QPaintEvent* event) override {
        QSpinBox::paintEvent(event);

        QStyleOptionSpinBox opt;
        initStyleOption(&opt);

        QRect upRect = style()->subControlRect(QStyle::CC_SpinBox, &opt, QStyle::SC_SpinBoxUp, this);
        QRect downRect = style()->subControlRect(QStyle::CC_SpinBox, &opt, QStyle::SC_SpinBoxDown, this);

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const bool upHover = (opt.activeSubControls & QStyle::SC_SpinBoxUp);
        const bool downHover = (opt.activeSubControls & QStyle::SC_SpinBoxDown);

        const QColor upColor = upHover ? design::color(design::kAccentInteractive) : design::color(design::kTextSecondary);
        const QColor downColor = downHover ? design::color(design::kAccentInteractive) : design::color(design::kTextSecondary);

        // Chevron Up (^)
        p.setPen(QPen(upColor, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPointF upCenter = upRect.center();
        QPainterPath upArrow;
        upArrow.moveTo(upCenter.x() - 3.5, upCenter.y() + 1.5);
        upArrow.lineTo(upCenter.x(), upCenter.y() - 2.0);
        upArrow.lineTo(upCenter.x() + 3.5, upCenter.y() + 1.5);
        p.drawPath(upArrow);

        // Chevron Down (v)
        p.setPen(QPen(downColor, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPointF downCenter = downRect.center();
        QPainterPath downArrow;
        downArrow.moveTo(downCenter.x() - 3.5, downCenter.y() - 1.5);
        downArrow.lineTo(downCenter.x(), downCenter.y() + 2.0);
        downArrow.lineTo(downCenter.x() + 3.5, downCenter.y() - 1.5);
        p.drawPath(downArrow);
    }
};

// ---- SettingItemCard --------------------------------------------------------

SettingItemCard::SettingItemCard(const SettingDefinition& def, SettingsStore& store,
                                  ProjectSettingsAccess projectAccess, QWidget* parent)
    : QFrame(parent), def_(def), store_(store), projectAccess_(std::move(projectAccess)) {
    setupUi();
    refreshValue(StoreScope::User);
}

void SettingItemCard::setupUi() {
    // Card and field skins come from the view's one sheet (settingsStyleSheet()).
    setObjectName(QStringLiteral("settingItemCard"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(design::kSpace3, design::kSpace3, design::kSpace3, design::kSpace3);
    mainLayout->setSpacing(design::kSpace1);

    // 1. Top row: Breadcrumb + Modified chip + Reset button
    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);

    // Storage indicator: User- and Project-scoped cards share a page, so each
    // card states where its value lives.
    QString storageLabel;
    switch (def_.scope) {
        case SettingScope::ProjectOnly: storageLabel = QStringLiteral("Project (.sdp)"); break;
        case SettingScope::WorkspaceOnly: storageLabel = QStringLiteral("Workspace"); break;
        case SettingScope::UserOnly: storageLabel = QStringLiteral("User"); break;
        case SettingScope::Overridable: storageLabel = QStringLiteral("User/Workspace"); break;
    }
    auto* metaLabel = new QLabel(QStringLiteral("%1 › %2 · %3").arg(def_.category, def_.key, storageLabel), this);
    metaLabel->setStyleSheet(recolored(design::kTypeMono, "text-disabled"));
    topRow->addWidget(metaLabel);

    topRow->addStretch(1);
    topRow->setSpacing(design::kSpace2);

    modifiedIndicator_ = new QLabel(QStringLiteral("● Modified"), this);
    modifiedIndicator_->setStyleSheet(recolored(design::kTypeLabel, "accent-interactive"));
    modifiedIndicator_->hide();
    topRow->addWidget(modifiedIndicator_);

    resetButton_ = new QPushButton(QStringLiteral("Reset"), this);
    resetButton_->setToolTip(QStringLiteral("Reset to default value"));
    resetButton_->setStyleSheet(design::kSecondaryButtonStyle);
    resetButton_->hide();
    connect(resetButton_, &QPushButton::clicked, this, [this] {
        emit resetRequested(def_.key);
    });
    topRow->addWidget(resetButton_);

    mainLayout->addLayout(topRow);

    // 2. Title label
    auto* titleLabel = new QLabel(def_.title, this);
    titleLabel->setStyleSheet(design::kTypeSection);
    mainLayout->addWidget(titleLabel);

    // 3. Description label
    if (!def_.description.isEmpty()) {
        auto* descLabel = new QLabel(def_.description, this);
        descLabel->setStyleSheet(design::kTypeLabel);
        descLabel->setWordWrap(true);
        mainLayout->addWidget(descLabel);
    }

    // 4. Editor row
    auto* editorRow = new QHBoxLayout();
    editorRow->setContentsMargins(0, design::kSpace1, 0, 0);
    editorRow->setSpacing(design::kSpace2);

    switch (def_.type) {
        case SettingType::Bool: {
            boolEditor_ = new QCheckBox(QStringLiteral("Enabled"), this);
            connect(boolEditor_, &QCheckBox::toggled, this, [this](bool checked) {
                if (!updatingProgrammatically_) {
                    emit valueChanged(def_.key, checked);
                }
            });
            editorRow->addWidget(boolEditor_);
            break;
        }
        case SettingType::Int: {
            intEditor_ = new SettingSpinBox(this);
            intEditor_->setRange(static_cast<int>(def_.minimum), static_cast<int>(def_.maximum));
            intEditor_->setMinimumWidth(kIntFieldMinWidth);
            connect(intEditor_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int val) {
                if (!updatingProgrammatically_) {
                    emit valueChanged(def_.key, val);
                }
            });
            editorRow->addWidget(intEditor_);
            break;
        }
        case SettingType::Enum: {
            enumEditor_ = new QComboBox(this);
            for (const QString& opt : def_.enumOptions) {
                enumEditor_->addItem(opt);
            }
            enumEditor_->setMinimumWidth(kEnumFieldMinWidth);
            connect(enumEditor_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
                if (!updatingProgrammatically_) {
                    emit valueChanged(def_.key, text);
                }
            });
            editorRow->addWidget(enumEditor_);
            break;
        }
        case SettingType::DirPath: {
            stringEditor_ = new QLineEdit(this);
            stringEditor_->setMinimumWidth(kTextFieldMinWidth);
            connect(stringEditor_, &QLineEdit::editingFinished, this, [this] {
                if (!updatingProgrammatically_) {
                    emit valueChanged(def_.key, stringEditor_->text());
                }
            });
            editorRow->addWidget(stringEditor_);

            // Commit on directory-chosen, not per keystroke: the manifest is
            // written only once a real directory is picked.
            auto* browseButton = new QPushButton(QStringLiteral("Browse..."), this);
            browseButton->setStyleSheet(design::kSecondaryButtonStyle);
            connect(browseButton, &QPushButton::clicked, this, [this] {
                const QString chosen = QFileDialog::getExistingDirectory(
                    this, QStringLiteral("Choose %1").arg(def_.title), stringEditor_->text());
                if (chosen.isEmpty()) {
                    return;  // user canceled
                }
                stringEditor_->setText(chosen);
                if (!updatingProgrammatically_) {
                    emit valueChanged(def_.key, chosen);
                }
            });
            editorRow->addWidget(browseButton);
            break;
        }
        default: {
            stringEditor_ = new QLineEdit(this);
            stringEditor_->setMinimumWidth(kTextFieldMinWidth);
            connect(stringEditor_, &QLineEdit::editingFinished, this, [this] {
                if (!updatingProgrammatically_) {
                    emit valueChanged(def_.key, stringEditor_->text());
                }
            });
            editorRow->addWidget(stringEditor_);
            break;
        }
    }

    editorRow->addStretch(1);
    mainLayout->addLayout(editorRow);

    // A ProjectOnly card with no project open is disabled with a one-line
    // reason rather than hidden; refreshValue() toggles its visibility.
    if (def_.scope == SettingScope::ProjectOnly) {
        projectClosedHint_ = new QLabel(QStringLiteral("Open a project to edit this value -- it is saved in the project's .sdp file."), this);
        projectClosedHint_->setStyleSheet(recolored(design::kTypeLabel, "text-disabled"));
        projectClosedHint_->setWordWrap(true);
        projectClosedHint_->hide();
        mainLayout->addWidget(projectClosedHint_);
    }
}

QJsonValue SettingItemCard::projectValueOrDefault() const {
    if (!projectAccess_.isProjectOpen || !projectAccess_.isProjectOpen() || !projectAccess_.read) {
        return def_.defaultValue;
    }
    const QJsonValue val = projectAccess_.read(def_.key);
    if (val.isNull() || (val.isString() && val.toString().isEmpty())) {
        return def_.defaultValue;
    }
    return val;
}

void SettingItemCard::refreshValue(StoreScope currentScope) {
    const bool isProjectScope = (def_.scope == SettingScope::ProjectOnly);
    const bool projectOpen = projectAccess_.isProjectOpen && projectAccess_.isProjectOpen();
    QJsonValue val = isProjectScope ? projectValueOrDefault() : store_.get(def_.key, def_.defaultValue);

    updatingProgrammatically_ = true;
    if (boolEditor_ != nullptr) {
        boolEditor_->setChecked(val.toBool());
    } else if (intEditor_ != nullptr) {
        intEditor_->setValue(val.toInt());
    } else if (enumEditor_ != nullptr) {
        int idx = enumEditor_->findText(val.toString());
        if (idx >= 0) {
            enumEditor_->setCurrentIndex(idx);
        }
    } else if (stringEditor_ != nullptr) {
        stringEditor_->setText(val.toString());
    }
    updatingProgrammatically_ = false;

    // Inert, not hidden, with no project open.
    if (isProjectScope) {
        setEnabled(projectOpen);
        if (projectClosedHint_ != nullptr) {
            projectClosedHint_->setVisible(!projectOpen);
        }
    }

    updateModifiedState(currentScope);
}

void SettingItemCard::updateModifiedState(StoreScope currentScope) {
    bool showModified;
    if (def_.scope == SettingScope::ProjectOnly) {
        // No store entry: the project field itself is the explicit value, so
        // "modified" just means "differs from default".
        showModified = (projectValueOrDefault() != def_.defaultValue);
    } else {
        const bool isExplicit = store_.has(def_.key, currentScope);
        const bool differsFromDefault = (store_.get(def_.key, def_.defaultValue) != def_.defaultValue);
        showModified = isExplicit || differsFromDefault;
    }
    modifiedIndicator_->setVisible(showModified);
    resetButton_->setVisible(showModified);
}

bool SettingItemCard::matchesFilter(const QString& filterText, const QString& selectedCategory) const {
    if (!selectedCategory.isEmpty() && selectedCategory != QStringLiteral("All Settings")) {
        if (def_.category.compare(selectedCategory, Qt::CaseInsensitive) != 0) {
            return false;
        }
    }

    if (filterText.isEmpty()) {
        return true;
    }

    const QString lower = filterText.toLower();
    return def_.key.toLower().contains(lower) ||
           def_.title.toLower().contains(lower) ||
           def_.description.toLower().contains(lower) ||
           def_.category.toLower().contains(lower);
}

// ---- SettingsView -----------------------------------------------------------

SettingsView::SettingsView(SettingsStore& store, ProjectSettingsAccess projectAccess, QWidget* parent)
    : QWidget(parent), store_(store), projectAccess_(std::move(projectAccess)) {
    setupUi();
    populateCategories();
    populateCards();

    connect(&store_, &SettingsStore::settingChanged,
            this, &SettingsView::handleSettingChangedInStore);
}

void SettingsView::setupUi() {
    setObjectName(QStringLiteral("settingsView"));
    setAttribute(Qt::WA_StyledBackground, true);  // #settingsView's surface-0 page ground; cards sit on it at surface-2
    setStyleSheet(settingsStyleSheet());

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(design::kSpace4, design::kSpace4, design::kSpace4, design::kSpace4);
    rootLayout->setSpacing(design::kSpace3);

    // 1. Top Header Bar
    auto* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(design::kSpace2);

    // SVG-backed icon instead of a glyph prefix.
    auto* titleIconLabel = new QLabel(this);
    titleIconLabel->setPixmap(icons::assetPixmap("settings", design::kSectionIconSize,
                                                 design::color(design::kTextPrimary),
                                                 titleIconLabel->devicePixelRatioF()));
    headerLayout->addWidget(titleIconLabel);

    // The page title uses the panel-title ramp, not a display size.
    auto* titleLabel = new QLabel(QStringLiteral("Settings"), this);
    titleLabel->setStyleSheet(design::kTypeSection);
    headerLayout->addWidget(titleLabel);

    headerLayout->addSpacing(design::kSpace4);

    // Scope switcher: [ User ] [ Workspace ]
    auto* scopeGroup = new QButtonGroup(this);
    userScopeBtn_ = new QPushButton(QStringLiteral("User"), this);
    userScopeBtn_->setCheckable(true);
    userScopeBtn_->setChecked(true);
    workspaceScopeBtn_ = new QPushButton(QStringLiteral("Workspace"), this);
    workspaceScopeBtn_->setCheckable(true);

    // A toggle pair of Secondary buttons: the CHECKED state uses the state
    // layer, not the accent-brand fill reserved for the one Primary button.
    const QString scopeBtnStyle = design::kSecondaryButtonStyle +
                                  design::resolveRoles(QStringLiteral(
                                                           "QPushButton:checked {"
                                                           "  background: {surface-hover};"
                                                           "  color: {text-primary};"
                                                           "  border-bottom: %1 solid {accent-interactive};"
                                                           "}"))
                                      .arg(px(design::kSelectionBarWidth));
    userScopeBtn_->setStyleSheet(scopeBtnStyle);
    workspaceScopeBtn_->setStyleSheet(scopeBtnStyle);

    scopeGroup->addButton(userScopeBtn_, 0);
    scopeGroup->addButton(workspaceScopeBtn_, 1);
    headerLayout->addWidget(userScopeBtn_);
    headerLayout->addWidget(workspaceScopeBtn_);

    connect(userScopeBtn_, &QPushButton::clicked, this, [this] {
        setActiveScope(StoreScope::User);
    });
    connect(workspaceScopeBtn_, &QPushButton::clicked, this, [this] {
        setActiveScope(StoreScope::Workspace);
    });

    headerLayout->addSpacing(design::kSpace2);

    // Search bar
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(QStringLiteral("Search settings (e.g. grid, autosave)..."));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(kSearchFieldMinWidth);
    connect(searchEdit_, &QLineEdit::textChanged, this, &SettingsView::applyFilter);
    headerLayout->addWidget(searchEdit_, 1);

    // [ Open JSON ]
    openJsonBtn_ = new QPushButton(QStringLiteral("Open JSON"), this);
    openJsonBtn_->setToolTip(QStringLiteral("Open settings.json in external text editor"));
    openJsonBtn_->setStyleSheet(design::kSecondaryButtonStyle);
    connect(openJsonBtn_, &QPushButton::clicked, this, &SettingsView::handleOpenJson);
    headerLayout->addWidget(openJsonBtn_);

    rootLayout->addLayout(headerLayout);

    // 2. Main Two-Pane Splitter
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("settingsSplitter"));

    // Left pane: Category navigation list. Selected row uses the state layer,
    // not a Primary-style fill.
    categoryList_ = new QListWidget(this);
    categoryList_->setObjectName(QStringLiteral("settingsCategories"));
    categoryList_->setFixedWidth(kCategoryListWidth);
    connect(categoryList_, &QListWidget::currentTextChanged, this, &SettingsView::applyFilter);
    splitter->addWidget(categoryList_);

    // Right pane: Settings card scroll area (scroll bars: theme.cpp's app-wide style).
    scrollArea_ = new QScrollArea(this);
    scrollArea_->setObjectName(QStringLiteral("settingsScroll"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);

    cardsContainer_ = new QWidget(scrollArea_);
    cardsContainer_->setObjectName(QStringLiteral("settingsCards"));
    cardsLayout_ = new QVBoxLayout(cardsContainer_);
    cardsLayout_->setContentsMargins(design::kSpace3, 0, design::kSpace2, 0);
    cardsLayout_->setSpacing(design::kSpace2);

    emptySearchLabel_ = new QLabel(QStringLiteral("No matching settings found"), cardsContainer_);
    emptySearchLabel_->setStyleSheet(recolored(design::kTypeLabel, "text-disabled") +
                                     QStringLiteral("margin-top: %1;").arg(px(kEmptyStateTopMargin)));
    emptySearchLabel_->setAlignment(Qt::AlignCenter);
    emptySearchLabel_->hide();
    cardsLayout_->addWidget(emptySearchLabel_);

    cardsLayout_->addStretch(1);
    scrollArea_->setWidget(cardsContainer_);
    splitter->addWidget(scrollArea_);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(splitter, 1);
}

void SettingsView::populateCategories() {
    categoryList_->clear();
    categoryList_->addItem(QStringLiteral("All Settings"));

    const auto& cats = SettingsRegistry::instance().categories();
    for (const QString& cat : cats) {
        categoryList_->addItem(cat);
    }

    categoryList_->setCurrentRow(0);
}

void SettingsView::populateCards() {
    const auto& defs = SettingsRegistry::instance().allSettings();
    for (const auto& def : defs) {
        auto* card = new SettingItemCard(def, store_, projectAccess_, cardsContainer_);
        cards_.push_back(card);
        cardMap_.insert(def.key, card);

        // ProjectOnly bypasses SettingsStore and goes through the seam; the
        // store never learns about these two keys.
        connect(card, &SettingItemCard::valueChanged, this, [this, card](const QString& key, const QJsonValue& val) {
            if (card->definition().scope == SettingScope::ProjectOnly) {
                if (projectAccess_.write) {
                    projectAccess_.write(key, val);
                }
                card->refreshValue(currentScope_);
                return;
            }
            store_.set(key, val, currentScope_);
        });

        connect(card, &SettingItemCard::resetRequested, this, [this, card](const QString& key) {
            if (card->definition().scope == SettingScope::ProjectOnly) {
                // Reset means "clear the project field back to default" --
                // there is no store entry to remove for this scope.
                if (projectAccess_.write) {
                    projectAccess_.write(key, card->definition().defaultValue);
                }
                card->refreshValue(currentScope_);
                return;
            }
            store_.remove(key, currentScope_);
            if (auto* c = cardMap_.value(key, nullptr)) {
                c->refreshValue(currentScope_);
            }
        });

        // Insert before stretch
        cardsLayout_->insertWidget(cardsLayout_->count() - 1, card);
    }
}

void SettingsView::setActiveScope(StoreScope scope) {
    if (currentScope_ == scope) {
        return;
    }
    currentScope_ = scope;
    userScopeBtn_->setChecked(scope == StoreScope::User);
    workspaceScopeBtn_->setChecked(scope == StoreScope::Workspace);

    for (auto* card : cards_) {
        card->refreshValue(currentScope_);
    }
}

void SettingsView::setSearchQuery(const QString& query) {
    searchEdit_->setText(query);
}

void SettingsView::handleSettingChangedInStore(const QString& key, const QJsonValue&,
                                              const QJsonValue&, StoreScope) {
    if (auto* card = cardMap_.value(key, nullptr)) {
        card->refreshValue(currentScope_);
    }
}

void SettingsView::applyFilter() {
    const QString filterText = searchEdit_->text().trimmed();
    const QString selectedCat = categoryList_->currentItem() ? categoryList_->currentItem()->text() : QString();

    int visibleCount = 0;
    for (auto* card : cards_) {
        const bool match = card->matchesFilter(filterText, selectedCat);
        card->setVisible(match);
        if (match) {
            ++visibleCount;
        }
    }

    emptySearchLabel_->setVisible(visibleCount == 0);
}

void SettingsView::handleOpenJson() {
    emit openJsonRequested(currentScope_);

    QString path = (currentScope_ == StoreScope::Workspace) ? store_.workspacePath() : store_.userPath();

    if (path.isEmpty() && currentScope_ == StoreScope::User) {
        path = SettingsStore::defaultUserPath();
    }

    if (!path.isEmpty()) {
        QFileInfo info(path);
        if (!info.exists()) {
            store_.flushSync();
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

}  // namespace app
