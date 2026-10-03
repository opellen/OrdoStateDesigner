#pragma once

#include <memory>
#include <vector>

#include <QGraphicsScene>
#include <QString>
#include <QStringList>

#include <ordo/core/kernel.h>
#include <ordo/qt/view_model.h>

#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "infra/sim_clock.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"

namespace ordo::qt {
class ViewHost;
}

namespace app {

class CanvasView;
class CanvasPresenter;
struct AutoLayoutOptions;  // view/geometry/auto_layout.h

// Persistent per-session badge read by several UI locations at once (pane
// header, sidebar row, status bar chip), so `changed()` is a Qt signal that
// each consumer connects to independently.
//
// Lives on the session's own persistent ViewHost (badgeHost_), so it keeps
// reporting the active state even with zero panes bound.
class StatusBadgeAdapter : public ordo::qt::ViewModel {
    Q_OBJECT

public:
    StatusBadgeAdapter();

    void onRegister() override;

    // "● <ActiveStateName>" while Simulate mode is actively running(); the
    // literal word "idle" otherwise (Design mode, Simulate-but-never-run,
    // or Simulate-but-paused-with-nothing-yet-active). computeText() below
    // is the one place this rule is decided.
    QString text() const { return text_; }

signals:
    void changed();

private:
    void onActiveStateChanged(const events::ActiveStateChanged&);
    void onSimulationStarted(const events::SimulationStarted&);
    void onSimulationPaused(const events::SimulationPaused&);
    void onSimulationReset(const events::SimulationReset&);
    void onModeChanged(const events::ModeChanged&);

    void refresh();
    QString computeText() const;

    std::shared_ptr<MachineDocAgent> doc_;
    std::shared_ptr<SimulationAgent> sim_;
    QString text_ = QStringLiteral("idle");
};

// Session-lifetime outline feed for the MACHINES explorer tree: machine ->
// states -> outgoing-transition events, plus which state is live. Lives on the
// same persistent host as StatusBadgeAdapter and derives the outline fresh from
// the agents on every outline() call rather than caching.
class MachineOutlineAdapter : public ordo::qt::ViewModel {
    Q_OBJECT

public:
    struct OutlineEvent {
        quint64 transitionId = 0;
        QString label;  // event name, or "after Nms" for a pure delayed transition
    };
    struct OutlineState {
        quint64 stateId = 0;
        QString name;
        StateKind kind = StateKind::Normal;
        bool isInitial = false;  // stateId == Machine::initialStateId
        bool active = false;  // Simulate-and-running only, same formula as the badge
        QVector<OutlineEvent> events;
    };

    MachineOutlineAdapter();

    void onRegister() override;

    QVector<OutlineState> outline() const;

signals:
    // One coarse signal for every structural or live-state change -- the
    // consumer re-reads outline() wholesale.
    void outlineChanged();

private:
    // Subscribes EventT with a handler that only re-emits outlineChanged().
    template <typename EventT>
    void watch();

    std::shared_ptr<MachineDocAgent> doc_;
    std::shared_ptr<SimulationAgent> sim_;
};

// Session-lifetime live-validation feed for MainWindow's Problems tab. Same
// shape as MachineOutlineAdapter: problems() runs validate() against the live
// machine on every call, and problemsChanged() fires on exactly the facts
// validate() reads (description/tags are not among them, so not watched).
class MachineProblemsAdapter : public ordo::qt::ViewModel {
    Q_OBJECT

public:
    MachineProblemsAdapter();

    void onRegister() override;

    QVector<Problem> problems() const;

signals:
    // One coarse signal per validate()-relevant fact -- the consumer
    // re-reads problems() wholesale, same contract as outlineChanged().
    void problemsChanged();

private:
    template <typename EventT>
    void watch();

    std::shared_ptr<MachineDocAgent> doc_;
};

// One open machine document: its own Kernel, agents and commands; the same
// machine may be bound into several panes at once.
// Each binding gets its own QGraphicsScene: CanvasPresenter clears its scene on
// every snapshot round trip, so presenters sharing one would delete each
// other's items (use-after-free).
class DocumentSession {
public:
    // Boot-empty session: starts with an empty Machine. `manualClock` false =
    // real ~50ms QTimer; true = delayed transitions advance only via
    // advanceClock(), with no event loop needed (smoke runs).
    explicit DocumentSession(QString machineName, bool manualClock = false);

    // Starts already holding `machine`, restored before any command, view or
    // badge can react to it. Undo history is cleared after the restore, so undo
    // never crosses a document.
    DocumentSession(QString machineName, Machine machine, bool manualClock = false);
    ~DocumentSession();

    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;

    // Creates a fresh QGraphicsScene + ViewHost + CanvasPresenter bound to
    // `view` and this session's kernel. `view` must outlive the matching
    // detachView(); the caller owns it. Several different views may be attached
    // at once. Returns the presenter (non-owning, valid until detachView()).
    CanvasPresenter* attachView(CanvasView* view);

    // Reverses one attachView(view): destroys that binding's host and scene and
    // clears `view`'s scene pointer. Other bindings are untouched. No-op if
    // `view` was never attached.
    void detachView(CanvasView* view);

    ordo::core::Kernel& kernel() { return kernel_; }

    // What one on-demand auto layout did: states moved and labels whose ratio
    // changed (all zero means no undo entry), and labels it could not clear.
    // `ran` is false when refused (no GUI application to measure with, or no document).
    struct AutoLayoutRun {
        int movedStates = 0;
        int changedLabels = 0;
        int residuals = 0;
        bool ran = false;
    };

    // Lays the whole machine out with `options` and applies it as ONE undo
    // step: computes the plan here, where the canvas metrics live, and sends
    // it as ApplyLayoutPlanRequested. The Edit menu, the canvas menu and MCP
    // all come through this one door. Mode policy is the command's (a
    // simulating kernel applies nothing); callers gate on Design mode.
    AutoLayoutRun runAutoLayout(const AutoLayoutOptions& options);

    // The session's own name; unrelated to StatusBadgeAdapter::text().
    const QString& machineName() const { return machineName_; }
    void setMachineName(const QString& name);

    const QString& relativePath() const { return relativePath_; }
    void setRelativePath(const QString& relPath) { relativePath_ = relPath; }

    // Persistent per-session feeds: non-owning, alive for the whole session
    // regardless of view bindings.
    StatusBadgeAdapter* badge() const { return badge_; }
    MachineOutlineAdapter* outlineAdapter() const { return outline_; }
    MachineProblemsAdapter* problemsAdapter() const { return problems_; }

    // Writes the agent's CURRENT machine (not the last loaded one) to `osdPath`.
    // Returns false (and fills *error, if non-null) on any write failure.
    bool saveTo(const QString& osdPath, QString* error = nullptr);

    // Manual-clock test hook: synchronously advances this session's SimClock
    // by `elapsedMs`. Only meant for sessions constructed with manualClock=true.
    void advanceClock(int elapsedMs) { clock_.advanceTicks(elapsedMs); }

    // Labels the initial auto-layout could not place clear of everything, or -1
    // when it never ran for this session. Probe hook.
    int debugAutoLayoutResidualCount() const { return autoLayoutResidualCount_; }

private:
    struct ViewBinding {
        CanvasView* view = nullptr;              // non-owning -- PaneWidget owns the widget
        std::unique_ptr<QGraphicsScene> scene;   // declared before host: destroyed AFTER it
        std::unique_ptr<ordo::qt::ViewHost> host;  // holds the CanvasPresenter; destroyed first
    };

    QString machineName_;
    QString relativePath_;

    // Declaration order is reverse teardown order: kernel_ dies last, clock_
    // before it.
    ordo::core::Kernel kernel_;
    SimClock clock_;

    // Set from applyAutoLayout()'s result in the constructor; -1 = did not run.
    int autoLayoutResidualCount_ = -1;

    std::unique_ptr<ordo::qt::ViewHost> badgeHost_;
    StatusBadgeAdapter* badge_ = nullptr;            // owned by badgeHost_
    MachineOutlineAdapter* outline_ = nullptr;       // owned by badgeHost_ (added after badge_; LIFO clear)
    MachineProblemsAdapter* problems_ = nullptr;     // owned by badgeHost_ (added last; LIFO clear)

    std::vector<ViewBinding> viewBindings_;
};

// ---- Project-level persistence ----------------------------------------------
//
// Free functions, not DocumentSession methods: they operate on a whole project
// (a manifest plus N machines) and never touch a dialog, so MainWindow wraps
// them with its UI layer and the smoke run calls them directly.

// Loads `ossPath`'s manifest plus every machine file (resolved relative to the
// manifest's directory, in manifest order), one fresh DocumentSession per
// machine. Session name = loaded Machine::name, else the file's base name.
// All-or-nothing: the first failure returns an empty vector and fills *error
// (if non-null). `manualClock` is forwarded to every session.
std::vector<std::unique_ptr<DocumentSession>> loadProjectSessions(const QString& ossPath, Project* projectOut,
                                                                    QString* error, bool manualClock = false);

// Writes `sessions` as a manifest at `ossPath` (Project::name = its base name)
// plus one machine file per session next to it, named from sanitizeSnakeCase(
// machineName()) unless the session already has a relative path.
// `outputDir`/`rootNamespace` become the Project's own fields. Returns false
// (and fills *error) on the first failure.
bool saveProjectSessions(const QString& ossPath, const std::vector<DocumentSession*>& sessions,
                          const QString& outputDir, const QString& rootNamespace, Project* projectOut,
                          QString* error);

// ---- XState v5 interop ------------------------------------------------------
//
// Dialog-free import core, like loadProjectSessions() above: MainWindow wraps it
// with a file dialog and a diagnostics box; probes and the CLI call it directly.

struct XStateSessionImport {
    std::unique_ptr<DocumentSession> session;  // null on failure -- see error
    QStringList diagnostics;                   // one line per unmapped construct (may be non-empty on success)
    QString error;                             // set only when session is null
};

// Imports `jsonPath` into ONE fresh DocumentSession named after the imported
// Machine::name (the XState `id`, or "Imported" as a fallback).
XStateSessionImport importXStateNewSession(const QString& jsonPath, bool manualClock = false);

}  // namespace app
