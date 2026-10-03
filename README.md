# Ordo State Designer

Draw a state machine on a canvas, simulate it live, and generate the C++ that
runs it. Ordo State Designer is a C++20 / Qt 6 Widgets application built on
the [**ordo**](https://github.com/opellen/Ordo) framework (a separate
repository, consumed as a build dependency). Each open machine is its own document, and each document runs on
its own ordo `Kernel`.

What it covers:

- **Statecharts**: compound, parallel, final and history (shallow/deep)
  states; targetless, self, eventless (`always`), delayed (`after`) and
  periodic (`every`) transitions; multi-target transitions; machine-level
  (root) transitions; wildcard event descriptors (`*`, `prefix.*`).
- **Extended state**: typed context variables (Bool, Int, Double, String,
  Object, user-defined structs), guard expressions, assign actions
  (`count = count + 1`), `raise(...)`, typed event payloads, and invoked
  actors with `onDone`/`onError`.
- **A live simulator** that interprets the diagram directly, with trace,
  step-back, breakpoints and manual completion of invocations.
- **A C++ code generator**. The generated files include no Qt headers, and
  the transition core needs only the standard library.
- **Interop**: XState v5 JSON (import/export), W3C SCXML (import/export),
  PlantUML (export).

## Building

Requirements:

- CMake 3.24+, a C++20 compiler, Ninja (recommended).
- Qt 6 with the Core, Widgets, Network and Svg modules. The tested kit is
  Qt 6.11 with MinGW on Windows.
- A checkout of the [ordo framework](https://github.com/opellen/Ordo).

The repo-root [`CMakeLists.txt`](CMakeLists.txt) is the entry point. It looks
for ordo in this order and stops at the first match:

1. `-DORDO_DIR=<path>`
2. `third_party/ordo/`
3. `../ordo` (a sibling checkout)

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<Qt6 prefix>
cmake --build build
```

The executable is written to `build/state-designer.exe`. **Run it from the
repo root.** `--smoke` resolves machine files relative to the working
directory.

Build options:

| Option | Default | Effect |
|---|---|---|
| `ENABLE_LOGGING` | `ON` | Installs a Qt message handler that appends to `state-designer.log` in the system temp directory (`%TEMP%` on Windows) and echoes to stderr, and enables routing-trace diagnostics |
| `ENABLE_DEV_DEMO` | `OFF` | Opens two demo machines (Login Flow, Traffic Light) at startup. `--gui-probe` requires it |
| `SD_SKIP_COLOR_GATE` | `OFF` | Skips the color-literal check |

If LLVM `lld` is installed, the build links with it. Otherwise the default
linker is used.

## Command line

With no flags the GUI starts and reopens the last project, unless that is
turned off in Settings. `--no-restore` skips the reopen for one launch.

| Flag | What it does |
|---|---|
| `--smoke` | Headless self-check (see below); exit 0 on success |
| `--gui-probe` | Offscreen GUI walkthrough plus scripted scenarios; needs `ENABLE_DEV_DEMO=ON` |
| `--gui-probe-visible` | The same run in a visible window |
| `--gui-probe-only <a,b,...>` | Only the scenarios whose function name contains a term. Skipping changes later scenarios' starting state, so only the full run is a complete check |
| `--codegen-check [dir]` | Validates, generates and writes a sample machine (default `state-designer-codegen-check` in the system temp directory) |
| `--import <format> <in> [--save-project <out.sdm>] [--generate <dir>]` | Import, validate, then save and/or generate (at least one is required). `--import-xstate <in.json>` is shorthand for format `xstate-v5` |
| `--export <format> <in.sdm> <out>` | Export with format `xstate-v5`, `scxml` or `plantuml`. `--export-xstate <in.sdm> <out.json>` is shorthand |
| `--project <file.sdm> --generate <dir>` | Regenerate one saved machine's C++ |
| `--machine-drift-check <file.sdm> <dir>` | Regenerate to a scratch directory and byte-compare with `<dir>`; non-zero exit on any missing, extra or differing file |
| `--machine-doc-check <doc.md> <file.sdm>` | Compare a Markdown doc's marked transition table (and optional context table) with a machine file |
| `--resave-machine <in.sdm> [out.sdm]` | Load and save, upgrading to the current format version |
| `--crash-selftest[-stackoverflow]` | Crash on purpose to test the crash handler |

Import notes go to stderr and are not fatal. A headless import fails only on
an unreadable file or a validator Error. The CLI generators use the root
namespace `app::generated`.

## Self-checks

**`--smoke`** runs the headless suite in one process and stops at the first
failure. It covers editing and file round trips, undo, hierarchy, context and
invocations, expressions, the simulator, the code generator, the validator,
XState v5 and SCXML interop, auto-layout, multi-kernel shell isolation, and
the MCP bridge. A passing run prints one line starting with
`PASS: state-designer smoke (`.

**`--gui-probe`** opens the real shell (offscreen by default), splits a second
pane, and runs the Traffic Light demo in it. Two captures are taken about
1200 ms apart with no kernel sends in between, which shows the machine's
clock advancing on its own. A scenario stage then drives feature flows
through the shell's own affordances and fails on any assertion. Captures go
to the directory named in `probeCaptureDir()`
([`src/harness/probe_support.cpp`](src/harness/probe_support.cpp)).

## Source layout

```
CMakeLists.txt              build entry point: resolves ordo, adds src/
src/
  CMakeLists.txt            the state-designer target
  main.cpp                  flag parsing, log sink, crash handler, MainWindow boot
  constants/                app version, design tokens
  model/                    Machine data (machine.h); MachineDocAgent (owns one
                            Machine, mints ids, mutation choke points) and its
                            journal; SimulationAgent (the interpreter); UndoStore;
                            the edit / simulation / undo event vocabularies
  controller/               one Command per intent: edit policy, simulation,
                            undo capture (Transaction, UndoCaptureCommand), undo/redo
  infra/                    expression language (parse, type-check, evaluate, emit
                            C++); validator; code generator (flat + hierarchical);
                            .sdm/.sdp I/O; import/export registry with XState v5,
                            SCXML and PlantUML adapters; SimClock; background I/O;
                            settings; MCP bridge; crash handler
  view/
    canvas/                 CanvasView (QGraphicsView), CanvasPresenter (one per
                            pane x machine binding) and its delegates, inline
                            editing, context menus, interaction FSM wrappers
    items/                  QGraphicsItems: state, transition, handle, frame, note, ...
    geometry/               pure geometry: orthogonal edge router, pill ports,
                            anchors, layered auto-layout
    shell/                  MainWindow, DocumentSession, editor groups, panels
                            (Machines, Inspector, Logic, Trace), dialogs, theme
    generated/              generated code the app itself runs on (see below)
  harness/                  --smoke phases, --gui-probe scenarios, CLI modes
  machines/                 the app's own machines and test fixtures
  resources/                icons and Qt resource files
```

## Architecture

### One machine = one document = one `Kernel`

Opening a machine creates a `DocumentSession`
([`src/view/shell/document_session.h`](src/view/shell/document_session.h)).
Each session has its own ordo `Kernel`, its own `MachineDocAgent`,
`SimulationAgent` and `UndoStore`, its own command registrations, and its own
`SimClock`. Two open machines share no dispatcher, agent or command.

A pane is a *view binding*, not a document.
`DocumentSession::attachView(CanvasView*)` builds a new `QGraphicsScene`,
`ViewHost` and `CanvasPresenter` for that one pane on the session's kernel.
`detachView()` removes exactly that binding and leaves every other pane on the
session alone. The same machine can be shown in two panes with no
synchronization code: each pane has its own scene and presenter, and both
react to the one kernel's facts independently. Scenes are per binding, not per
session, on purpose. `CanvasPresenter::rebuildAll()` calls
`QGraphicsScene::clear()`, so two presenters on one scene would delete each
other's items.

A session outlives its panes. `StatusBadgeAdapter`, `MachineOutlineAdapter` and
`MachineProblemsAdapter` live on a persistent `ViewHost` owned by the session.
They keep feeding the status chip ("● <State>" while running, otherwise
"idle"), the Machines tree and the Problems list even when no pane shows the
machine. The session's `SimClock` also keeps firing delayed transitions with
no canvas open.

Anything that spans documents (the Machines panel, New Machine, Open/Save
Project, Import/Export) lives in `MainWindow`, above every kernel. It can read
one session and send an intent into another. No session holds a reference to
a sibling.

### Journaled undo with per-op facts

`MachineJournal` ([`src/model/machine_journal.h`](src/model/machine_journal.h))
is a non-owning observer on `MachineDocAgent`'s mutation choke points
(`stateAdded`, `stateRenamed`, `transitionRetargeted`, `stateDeleted`, ...).
Each notification carries before and after images. `Transaction`
([`src/controller/undo_capture.h`](src/controller/undo_capture.h)) is a
stack-local object, not an agent. There is one per mutating
`Command::execute()`:

- `begin()` attaches a capturing journal.
- `commit()` detaches the journal and pushes the captured ops onto
  `UndoStore`, which clears redo. It pushes only if something changed.
- `abort()` replays the ops backward and pushes nothing.

Every undo-worthy edit intent is registered as
`UndoCaptureCommand<RealCommand, EventT>`, so `RealCommand` never knows that
undo exists. An edit rejected by policy (for example any edit while
simulating, or a second unguarded transition for the same source and event)
calls no choke point, so it records nothing.

**Per-op facts are the view-sync mechanism.** `applyDelta()` replays a
captured delta through the same choke-point methods a live edit uses:
backward with before-images for undo, forward with after-images for redo.
Each call republishes the ordinary fact, so every view that is already
subscribed updates the normal way. No separate snapshot broadcast is needed.
Undo's reverse order also handles cascades. `deleteState()` deletes children
first, then the state's transitions, then the state itself. Reversed, that
restores the state before the transitions that reference it.

Simulation state is never undo-captured, because Reset owns it.
`UndoCommand`/`RedoCommand` and `MachineSnapshotRequested` are registered
plain. Undo is scoped to one session's kernel, so it never crosses a document
boundary.

### The simulator

`SimulationAgent` ([`src/model/sim_agent.h`](src/model/sim_agent.h)) reads
`MachineDocAgent`'s live topology and interprets it. It never runs generated
code. Its state is the full active configuration: every active state and its
ancestors, in document order.

- **Run** activates `Machine::initialStateId` (descending into compound and
  parallel states), seeds the context, and arms timers and invocations. After
  a Pause it resumes where it stopped.
- **Pause** freezes armed countdowns. **Reset** starts over like a fresh Run
  and leaves the machine running.
- **Firing order** follows XState v5: exit actions (deepest first), then the
  transition's own action, then entry actions (parent first). Exit and entry
  are bounded by the transition's least common compound ancestor. Setting
  `reenter` makes a transition exit and re-enter (SCXML `type="external"`).
- **Targetless and self-transitions**: a targetless transition runs only its
  action. Nothing exits or enters, and timers are untouched. A
  self-transition exits and re-enters its subtree, so entry and exit actions
  run and timers re-arm.
- **Event selection**: for each active leaf state, the simulator checks the
  state itself, then its ancestors, and takes the first matching transition
  whose guard passes. Exact descriptors are tried before `prefix.*`, and
  `prefix.*` before `*`. Ties go to document order. Leaves in parallel regions
  can fire together as one macrostep. Machine-level (root) transitions are a
  fallback, used only when no state-level candidate fires. An unmatched event
  is ignored silently.
- **Guards**: a blank guard passes. A guard that is one bare identifier is a
  named hook. Its result is a toggle in the Inspector, and it defaults to
  `true`. Anything else is an expression. It is parsed, type-checked and
  evaluated against the live context. An undecidable expression evaluates to
  false and adds a trace line.
- **Actions**: `target = expr` is an assign to a context variable. It runs
  before the target state is entered, so entry actions see the new value but
  the guard that chose the transition does not. `raise(E)` queues an internal
  event. `sendTo(...)` and `sendParent(...)` are recorded in the trace. Any
  other text is a named action hook.
- **Eventless and raised events**: after each macrostep, `always` transitions
  and raised events are processed until the machine settles (with a limit of
  100 microsteps).
- **Delayed transitions** (blank event, `delayMs > 0`) arm when their source
  state is entered. Their guard is checked when they fire. Leaving the state
  cancels them. `every` transitions repeat. Root-level delays are armed once,
  at start.
- **Invocations**: the simulator runs no real service. It tracks which
  invocations are live, and the Inspector's Done/Error buttons complete one.
  That fires `done.invoke.<id>` or `error.platform.<id>`, with `output` or
  `error` in scope for that macrostep.
- **Back** replays; it does not reverse-execute. It rebuilds the initial
  activation and replays the first *N-1* of *N* macrosteps through the same
  `fireMicrostep()` a live firing uses. Guards are not re-checked, because
  only the callers of `fireMicrostep()` check guards.

## Code generation

`generate()` ([`src/infra/code_generator.h`](src/infra/code_generator.h)) is a
pure function. It takes a `Machine` and a root namespace and returns a list of
files with their text. It does no I/O, so the Inspector's Code tab can call
it on every change. The generated namespace is
`<rootNamespace>::<machine_snake_case>`. Coordinates never appear in generated
code.

Generate C++ (F9) runs the validator first. Any Error blocks writing, and
Warnings alone do not. If a project with an output directory is open, files
go to `<outputDir>/generated/<machine>/`. Otherwise the app asks for a
directory.

| File | Contents |
|---|---|
| `<m>_types.h` | only if the machine declares struct types |
| `<m>_state.h` | the state `enum class` + `toString()` |
| `<m>_events.h` | one struct per distinct event (with a `payload` member for typed events), plus `StateChanged{from, to}` |
| `<m>_hooks.h` | the `Context` struct (when the machine has context variables), `<M>Guards`, `<M>Actions`, `<M>Scheduler` (only with delayed transitions), `<M>Invocations` (only with invokes) |
| `<m>_core.h` | `<M>Core`: the whole transition logic. **Standard library only**, no ordo and no Qt |
| `<m>_agent.h` | `<M>StateAgent : ordo::core::Agent`, which owns a core and republishes its state changes as `StateChanged` |
| `<m>_commands.h` | one ordo `Command` per distinct event, each delegating to the agent's core |
| `<m>_bootstrap.h` | `register<M>()` / `unregister<M>()` |

A machine gets the **hierarchical** core if any state has a parent or any
transition has more than one target. Otherwise it gets the **flat** core. The
flat core exposes `state()`, and `register<M>()` arms delayed transitions and
invocations explicitly. The hierarchical core exposes `isActive(state)`
(parallel regions can have several active leaves) and arms everything in its
constructor.

Every generated file starts with a banner: *"Generated by Ordo State Designer
-- regenerated wholesale on every [Generate C++] click. DO NOT EDIT"*. Beside
the generated files, a `domain/` subdirectory receives starting-point stubs
(`<m>_hooks_impl.h`, `<m>_bootstrap.cpp`). They are written only if absent and
never overwritten.

### The firing order lives in one place

`<m>_core.h` holds the whole transition contract: the v5 order, first passing
candidate wins, all guards failing is a silent no-op, targetless runs only its
action. It depends on nothing but the hook interfaces and the standard
library. Expression guards and assign actions are compiled inline against
`Context`, and bare-identifier guards and named actions become hook methods.
The ordo layer is an adapter: the agent turns the core's state-change
callback into one `StateChanged` fact, and each command resolves the agent
and calls the matching core method. Code that is not an ordo app (a view-local
interaction FSM, a test) can embed the core directly.

The `<M>Scheduler` hook
(`virtual void scheduleAfter(int delayMs, std::function<void()> fire) = 0;`)
lets the caller choose the timer (a `QTimer`, or a manual clock in tests).
The core re-checks its state and guard when a timer fires, so a late or stale
callback does nothing. `<M>Invocations` gets a `std::stop_token` plus
`onDone`/`onError` callbacks. Its header comment states the threading
contract: completions must reach the machine's thread before they are called.

### Drift detection is the C++ compiler

The split between skeleton and logic is XState's `.provide()` pattern mapped
onto C++:

- The generated files belong to the generator and are fully overwritten on
  each run.
- Your `Guards`/`Actions` (and `Scheduler`/`Invocations`) implementations are
  ordinary subclasses that `register<M>()` takes by reference. Regenerating
  never touches them.
- Rename a guard or action on the canvas and regenerate: the pure-virtual
  method's name changes and your subclass no longer implements it, so the
  build fails at that spot. XState's Typegen does the same job with a
  generated type. Here it is done with a generated interface.

### The app runs on its own generated code

[`src/view/generated/`](src/view/generated/) holds the output for two machines
the app uses itself: the canvas interaction machine
([`src/machines/canvas-interaction.sdm`](src/machines/canvas-interaction.sdm),
hierarchical) and the note editor
([`src/machines/note-editor.sdm`](src/machines/note-editor.sdm)). The canvas
and note-editor FSM wrappers in `src/view/canvas/` embed the generated
`*_core.h` directly, with no kernel. `--smoke` regenerates the canvas machine
and fails unless the result is byte-identical to the committed headers. For
either machine the equivalent check is:

```
state-designer --machine-drift-check src/machines/note-editor.sdm src/view/generated
```

## File formats

- **`.sdm`**: one machine as JSON. It holds topology, geometry (state
  positions, label offsets, manual bendpoints), context variables, struct
  types, notes and colors. Output is byte-stable, so files diff cleanly in
  git.
- **`.sdp`**: a project manifest as JSON. It lists the machine files
  (relative to the manifest), the code output directory and the root
  namespace. Opening a project creates one `DocumentSession` per listed
  machine. Saving writes the manifest plus one `.sdm` per session.

Both files carry a `formatVersion` (currently 10). The loader reads older
versions, and `--resave-machine` rewrites a file in the current version.
A single `.sdm` can be opened with File ▸ Import....

## Import and export

File ▸ Import... and File ▸ Export... use one format registry
([`src/infra/machine_io.h`](src/infra/machine_io.h)). The export dialog
writes the active machine to a file or copies the text to the clipboard. For
SCXML and PlantUML it can also write every machine in the project, one file
each.

| Format | Import | Export |
|---|---|---|
| [XState v5](https://stately.ai/docs/xstate) machine config JSON | yes | yes |
| [W3C SCXML](https://www.w3.org/TR/scxml/) | yes | yes (optional layout metadata and actions) |
| PlantUML state diagram | no | yes |

**XState v5**: the converter is
[`src/infra/xstate_v5_io.h`](src/infra/xstate_v5_io.h). It reads and writes
what `createMachine()` accepts, including compound, parallel, final and
history states, `always`, `after`, `reenter`, context, guard expressions,
assigns and nested `invoke`. Names are the identity across the boundary.
Numeric ids never cross it. Geometry is stored under `meta.ordo`, and a file
without geometry (for example one written by hand or by Stately) is
auto-laid-out when it is opened in the GUI. Machine-level `on`/`after`
handlers become root transitions, shown as pills on the machine frame. Any
construct that cannot be mapped is reported with its JSON path, never dropped
silently. Export refuses a machine with blank or duplicate state names,
because XState keys states by name. The smoke suite checks that export ->
import -> export is byte-identical.

## MCP bridge

At startup the app runs `McpRuntimeServer`
([`src/infra/mcp_runtime_server.h`](src/infra/mcp_runtime_server.h)). It
listens on a local socket named `state-designer-live` (a named pipe on
Windows) for line-delimited JSON-RPC 2.0 and handles requests on the Qt main
thread. The methods cover:

- inspecting the active machine, the selection and the simulation status;
- capturing the canvas;
- creating and editing machines (states, transitions, context variables,
  invocations);
- auto layout;
- stepping and resetting the simulation, and completing invocations.

## Controls

**Canvas**:

- Double-click empty space to add a state (inside a compound state, it becomes
  a child).
- Drag a selected state's handle onto another state to create a transition.
- Drag an event pill to move its label; the offset is saved.
- Drag on empty canvas to rubber-band select, middle-drag to pan, and use the
  wheel to zoom (0.15x to 6x).
- `F` zooms to the selection, `Space` toggles the code lens,
  `Delete`/`Backspace` deletes, and `Escape` aborts a drag without committing.
- Right-click opens context menus (child state, self/targetless transition,
  machine event, note, auto layout, ...).

**Keyboard** (the focused pane's machine):

| Keys | Action |
|---|---|
| `F5` / `F6` / `F7` | Run / Pause / Reset |
| `F8` | Back to Design mode |
| `Ctrl+B` | Step back |
| `Ctrl+Z` / `Ctrl+Y` or `Ctrl+Shift+Z` | Undo / Redo |
| `F9` | Generate C++ |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` | New Machine / Open Project / Save / Save Project As |
| `Ctrl+Shift+O` | Import |
| `Ctrl+Shift+L` | Edit ▸ Auto Layout... |
| `Ctrl+,` | Settings |

## Contributing

Issues are welcome — bugs, questions, ideas, disagreements. Pull requests are
not accepted; the reasons and what a useful report contains are in
[CONTRIBUTING.md](CONTRIBUTING.md).

## License

Ordo State Designer is licensed under the GNU Affero General Public License
v3.0. See [`LICENSE`](LICENSE) for the full text. A commercial license is
available for uses the AGPL does not fit; write to <opellen.dev@gmail.com>
(details in [CONTRIBUTING.md](CONTRIBUTING.md#licensing)).
