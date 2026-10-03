#pragma once

// Helpers shared by two or more harness translation units: headless command
// bootstrap, probe capture, pill-port invariants, scene item lookups and the
// topology comparison. Single-TU helpers stay static in their own .cpp.
//
// Signatures only forward-declare app:: types; each .cpp that dereferences one
// includes the real header.

#include <QtGlobal>

class QGraphicsScene;
class QImage;
class QRect;
class QRectF;
class QString;
class QWidget;
struct QPointF;

namespace ordo::core {
class Kernel;
}

namespace app {
class EditorView;
class MainWindow;
class MachineFrameItem;
class NoteItem;
class SimClock;
class StateItem;
struct Machine;
struct PortAssignment;
}  // namespace app

// ---- headless command bootstrap (paired register/remove) ------------------
void registerEditCommands(ordo::core::Kernel& kernel);
void removeEditCommands(ordo::core::Kernel& kernel);
void registerSimCommands(ordo::core::Kernel& kernel, app::SimClock& clock);
void removeSimCommands(ordo::core::Kernel& kernel);
void registerUndoPhaseCommands(ordo::core::Kernel& kernel);
void removeUndoPhaseCommands(ordo::core::Kernel& kernel);

// ---- probe capture helpers --------------------------------------------------
QString probeCaptureDir();
bool writeProbeImage(const QImage& image, const char* baseName);
bool saveWidgetCapture(QWidget* widget, const char* fileName);
bool saveSceneCapture(app::EditorView* pane, const char* fileName);
bool saveSceneRegionCapture(app::EditorView* pane, const QRectF& region, const char* fileName);
bool saveWidgetRegionCapture(QWidget* widget, const QRect& region, const char* fileName);

// The login-flow demo machine's Authenticating state id (the third state
// buildLoginFlowMachine() adds).
constexpr quint64 kAuthenticatingStateId = 2;

// Blocks for `ms` of wall-clock time while pumping events. processEvents()
// alone only settles synchronous fallout; timers (e.g. MinimapView's ~60ms
// throttled re-fit) need real time to pass.
void pumpEventsFor(int ms);

// The port invariant's perpendicularity/half-shape checks.
bool portRunIsPerpendicular(QPointF from, QPointF fromDir, QPointF to, QPointF toDir);
bool portAssignmentHoldsInvariant(const app::PortAssignment& ports, const QRectF& pillRect, const char* what);

// Topology-only Machine equality (nextId excluded).
bool sameTopology(const app::Machine& a, const app::Machine& b);

// Scene item lookups by domain id.
app::StateItem* findStateItemById(QGraphicsScene* scene, quint64 id);
app::NoteItem* findNoteItemById(QGraphicsScene* scene, quint64 id);
app::MachineFrameItem* findFrameItem(QGraphicsScene* scene);
