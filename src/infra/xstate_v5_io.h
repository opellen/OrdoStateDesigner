#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "model/machine.h"

namespace app {

// XState v5 machine-config interop: pure converters between Machine and the JSON
// Stately's "Export -> JSON" emits / `createMachine()` accepts. Names are the identity
// (numeric ids never cross); unmappable constructs are diagnosed, never silently
// dropped. Geometry rides `meta.ordo`. Context, guard expressions and assigns map to
// v5's forms via the expression Ast (export -> import -> export is byte-identical);
// invoke maps to nested `invoke`, with onDone/onError read from the nested or flat form.

struct XStateExportResult {
    QJsonObject json;
    // Non-fatal notes: constructs Ordo holds that XState has no slot for
    // (a decorative delayMs on an eventful transition, a dead blank-event
    // transition) -- one line each, dropped from the JSON but reported.
    QStringList diagnostics;
    bool ok = true;   // false: names cannot key the export (blank/duplicate)
    QString error;    // set when !ok
};

// Machine -> XState v5 config. Refuses (ok=false) when a state name is
// blank or two states share a trimmed name -- XState keys states by name,
// so the export would be ambiguous (the validator's sanitized-collision
// check does not cover raw-name duplicates; this is the export-side gate).
XStateExportResult machineToXStateJson(const Machine& machine);

// Writes machineToXStateJson's result as pretty JSON. Returns false and
// fills *error on a refused export or an IO failure; *diagnostics (when
// non-null) receives the notes either way.
bool exportXStateFile(const Machine& machine, const QString& path, QStringList* diagnostics = nullptr,
                       QString* error = nullptr);

struct XStateImportResult {
    Machine machine;
    // One line per construct that could not be mapped, each with its JSON
    // path (`states.Paying.always: always (eventless) has no Phase-1
    // equivalent -- dropped`). Non-empty diagnostics do not mean failure: the
    // import is partial, everything mappable is in `machine`.
    QStringList diagnostics;
    bool ok = true;   // false: structural failure only (no `states` object / unreadable file)
    QString error;    // set when !ok
};

// XState v5 config -> Machine. Fresh ids are minted in document order,
// states first, then transitions (MachineDocAgent's single-counter
// discipline) -- Qt's QJsonObject iterates keys alphabetically, so
// "document order" is deterministic key order, same file same Machine.
// Geometry comes from `meta.ordo` where present; states without it stay at the
// origin for the caller to lay out.
XStateImportResult machineFromXStateJson(const QJsonObject& json);

// Reads `path` and runs machineFromXStateJson. ok=false additionally on an
// unreadable file or non-object root.
XStateImportResult importXStateFile(const QString& path);

}  // namespace app
