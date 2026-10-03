#pragma once

// Guard/action expression unit. The AST is never stored in the model: guards
// stay plain strings and are parsed on demand, with no cache to go stale.
// Pure: no widgets, no ordo/core, no model mutation; model/machine.h is the
// only project include (for the context schema).

#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>

#include <vector>

#include "model/machine.h"

namespace app::expr {

// Arity is structural: a malformed operator application cannot be built, so
// typeCheck() never has to police it.
enum class NodeKind {
    BoolLiteral,
    IntLiteral,
    DoubleLiteral,
    StringLiteral,
    Identifier,
    Not,
    Negate,
    And,
    Or,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    Call,
    MemberAccess
};

// One AST node in a flat arena; `lhs`/`rhs` are indices into Ast::nodes, so an
// Ast copies by value. sourceOffset/sourceLength are the span in the ORIGINAL
// text, which renameIdentifierInSource() splices so a rename never reformats.
struct Node {
    NodeKind kind = NodeKind::BoolLiteral;
    int lhs = -1;              // child index into Ast::nodes; -1 = none
    int rhs = -1;              // child index into Ast::nodes; -1 = none
    std::vector<int> extraArgs; // extra argument indices for Call (index 2+)
    QString text;              // Identifier name / StringLiteral value / Call function name / MemberAccess member
    qint64 intValue = 0;       // IntLiteral
    double doubleValue = 0.0;  // DoubleLiteral
    bool boolValue = false;    // BoolLiteral
    int sourceOffset = 0;      // span in the ORIGINAL string
    int sourceLength = 0;
};

// An arena plus the root's index; `root == -1` means no tree (failed parse).
struct Ast {
    std::vector<Node> nodes;
    int root = -1;
};

// Mirrors ContextType one-for-one; also types subexpressions, which have no
// ContextVariable behind them.
enum class ValueType { Bool, Int, Double, String, Object };

// A tagged value produced by evaluate(). Object values carry a QVariantMap.
struct Value {
    ValueType type = ValueType::Bool;
    bool boolValue = false;
    qint64 intValue = 0;
    double doubleValue = 0.0;
    QString stringValue;
    QVariantMap objectValue;

    static Value fromBool(bool value);
    static Value fromInt(qint64 value);
    static Value fromDouble(double value);
    static Value fromString(const QString& value);
    static Value fromObject(const QVariantMap& value);

    // Int and Double mix freely; every numeric comparison promotes through this.
    double asDouble() const;

    bool operator==(const Value&) const = default;
};

// True when `type` is Int or Double -- the "numeric" of the type rules, the
// only pair of types allowed to mix.
bool isNumeric(ValueType type);

ValueType fromContextType(ContextType type);

// The payload identifier in scope for a row: `output` (onDone, typed by the
// state's invokeOutputType), `error` (onError, always String) or `event` (a
// typed event). It is an ordinary Identifier, resolved ahead of the context
// schema. A blank `name` means no payload in scope.
enum class PayloadKind {
    None,
    Output,
    Error,
    Event,
};

struct PayloadBinding {
    PayloadKind kind = PayloadKind::None;
    QString name;
    ValueType type = ValueType::Int;
    QString structTypeName;
    const StructDefinition* structDef = nullptr;

    static PayloadBinding none();
    static PayloadBinding forOnDone(ContextType type);
    static PayloadBinding error();
    static PayloadBinding forEvent(const QString& payloadType, const QVector<StructDefinition>& structDefs = {});
};

// `position` is a CHARACTER offset into the source, so the inspector can
// underline exactly the offending token rather than the whole line. On a
// failure at end-of-input it is source.size().
struct ParseResult {
    bool ok = false;
    Ast ast;
    QString message;
    int position = 0;
};

// One typeCheck finding. Same `position` contract as ParseResult: the
// character offset of the subexpression at fault, taken from the node's
// recorded span.
struct TypeProblem {
    QString message;
    int position = 0;
};

// A guard whose whole text is ONE bare identifier is a named hook; every
// consumer asks this before parse(). Deliberately schema-independent, so adding
// a context variable never changes an existing guard's meaning. Consequence: a
// Bool variable is tested as `locked == true` or `!locked`, never bare.
bool isBareIdentifier(const QString& source);

// An assign form `<target> = <valueSource>`. parseAssignForm() is the only
// place that tells '=' apart from '==', '!=', '<=', '>='; never re-derive it.
// !ok with a blank message: no top-level '=', a plain action hook. !ok with a
// message: a malformed assign (e.g. `count + 1 = 2`), reported as such.
// `valueSource` is left unparsed; callers parse() it when they need an Ast.
struct AssignForm {
    bool ok = false;      // false = not an assign; the caller falls back to a named action hook
    QString target;       // the context variable's name or member path (e.g. "count" or "user.age", already trimmed)
    QString valueSource;  // the RHS, UNPARSED -- the caller parse()s it to get an Ast
    QString message;      // when !ok BUT the string clearly meant to be an assign: why it isn't valid
    int position = 0;

    [[nodiscard]] QString rootTarget() const {
        const int dot = target.indexOf(QLatin1Char('.'));
        return dot == -1 ? target : target.left(dot);
    }
    [[nodiscard]] QString memberPath() const {
        const int dot = target.indexOf(QLatin1Char('.'));
        return dot == -1 ? QString() : target.mid(dot + 1);
    }
    [[nodiscard]] bool isMemberAssign() const {
        return target.contains(QLatin1Char('.'));
    }
};

// Splits on the top-level '=' (not part of a comparison operator, not inside a
// quoted string). A lone '=' is a lex error in guards, so no valid guard can
// classify as an assign. Target: an identifier or dotted member path; the RHS
// must be non-blank; a second top-level '=' is malformed.
AssignForm parseAssignForm(const QString& source);

// A raise form `raise(EventName)` or `raise ( EventName )`.
// Enqueues an event onto the internal microstep queue.
struct RaiseForm {
    bool ok = false;      // false = not a raise form; fall back to assign or named hook
    QString event;        // the event name to raise (already trimmed)
    QString message;      // when !ok BUT the string clearly meant to be a raise: why it isn't valid
    int position = 0;
};

// Parse an action string for `raise(EventName)`.
// If `source` starts with `raise` followed by `(`, it classifies as an attempted raise.
// Returns `ok = true` with trimmed `event` if valid, or `ok = false` with descriptive `message`.
RaiseForm parseRaiseForm(const QString& source);

// A sendTo form `sendTo(target, EventName)`.
// Sends an event to an invoked actor.
struct SendToForm {
    bool ok = false;      // false = not a sendTo form; fall back to subsequent forms or named hook
    QString target;       // target actor ID / invokeId (already trimmed)
    QString event;        // the event name to send (already trimmed)
    QString message;      // when !ok BUT clearly intended to be sendTo: syntax error message
    int position = 0;
};

// Parse an action string for `sendTo(target, EventName)`.
// Returns `ok = true` with trimmed `target` and `event` if valid, or `ok = false` with descriptive `message`.
SendToForm parseSendToForm(const QString& source);

// A sendParent form `sendParent(EventName)`.
// Sends an event to the parent machine.
struct SendParentForm {
    bool ok = false;      // false = not a sendParent form; fall back to subsequent forms or named hook
    QString event;        // the event name to send to parent (already trimmed)
    QString message;      // when !ok BUT clearly intended to be sendParent: syntax error message
    int position = 0;
};

// Parse an action string for `sendParent(EventName)`.
// Returns `ok = true` with trimmed `event` if valid, or `ok = false` with descriptive `message`.
SendParentForm parseSendParentForm(const QString& source);

// Time trigger declaration: `after <duration>` or `every <interval>`.
enum class TimeTriggerKind {
    None,
    After,
    Every
};

struct TimeTrigger {
    TimeTriggerKind kind = TimeTriggerKind::None;
    int durationMs = 0;
    QString rawDuration;
    bool ok = false;
    QString message;
    int position = 0;

    [[nodiscard]] bool isAfter() const { return kind == TimeTriggerKind::After; }
    [[nodiscard]] bool isEvery() const { return kind == TimeTriggerKind::Every; }
};

// Parse a trigger string for `after <duration>` or `every <interval>`.
// Supports suffixes "ms", "s", "m", "min", "us", or bare numeric (treated as ms).
// Supports decimals (e.g. "1.5s" -> 1500ms, "0.5s" -> 500ms).
// Also supports legacy parenthesised format e.g. "(after 500 ms)".
TimeTrigger parseTimeTrigger(const QString& source);

// Composite transition label parsing (Trigger [guard] / action).
struct TransitionLabelForm {
    bool ok = true;
    QString event;           // Trigger string, e.g. "after 10s", "CLICK", "always", or empty
    TimeTrigger timeTrigger; // Parsed time trigger if trigger is a time trigger
    QString guard;           // Extracted guard string without brackets, e.g. "x > 0"
    QString action;          // Extracted action string without leading '/', e.g. "count = count + 1"
    bool hasEvent = false;
    bool hasGuard = false;
    bool hasAction = false;
    bool always = false;
};

// Parse a full transition label string, separating trigger/event, [guard], and / action.
TransitionLabelForm parseTransitionLabel(const QString& source);

// Parse `source` into an Ast. Never throws, never partially reports: on
// failure `ok` is false, `ast.root` is -1, and message/position locate the
// first problem.
ParseResult parse(const QString& source);

// Resolves every Identifier (against `payload` first, then `schema`) and
// applies the type rules bottom-up. Returns EVERY finding; empty means
// well-typed AND Bool at top level.
QVector<TypeProblem> typeCheck(const Ast& ast, const QVector<ContextVariable>& schema,
                                const PayloadBinding& payload = PayloadBinding(),
                                const QVector<StructDefinition>& structDefs = {});

// isBareIdentifier -> parse -> typeCheck, returning the first finding; empty
// means valid. Blank ("no guard") and bare-identifier (hook) sources are valid
// without parsing. `position` may be nullptr.
QString validateGuardSource(const QString& source, const QVector<ContextVariable>& schema, int* position,
                             const PayloadBinding& payload = PayloadBinding(),
                             const QVector<StructDefinition>& structDefs = {});

// Evaluates against `values` (identifier -> current value). Sets `ok` false,
// without throwing, on a missing identifier, an unsupported value type or an
// ill-typed tree; the result is then meaningless. Defensive even after
// typeCheck(): the live context map can drift from the schema between edits.
Value evaluate(const Ast& ast, const QVariantMap& values, bool* ok);

// Canonical infix: minimal parentheses, single spaces around binary operators,
// none after a unary one. For trees with no source text (XState import).
QString print(const Ast& ast);

// Renders `ast` as one C++ expression against a Context instance named
// `contextExpr`. An Identifier emits `<contextExpr>.<name>` verbatim (the
// validator guarantees valid, unique names), except those in `bareIdentifiers`
// (payload parameters such as `output`/`error`), which emit bare. Every
// non-leaf operand is parenthesized explicitly (see emitCppNode()). Empty
// string for a rootless tree; the Ast is assumed already type-checked.
QString emitCpp(const Ast& ast, const QString& contextExpr, const QSet<QString>& bareIdentifiers = QSet<QString>());

// Rewrites only whole Identifier tokens named `before`, splicing the original
// text right-to-left so earlier offsets stay valid; string literals and
// formatting survive. Returns `source` unchanged if it does not parse or is a
// bare identifier (a hook name, not a context reference).
QString renameIdentifierInSource(const QString& source, const QString& before, const QString& after);

// Check whether `source` text references `identifier` as a distinct identifier/token.
// Used for variable cross-highlighting across transitions (guards and actions).
// Handles assign forms, AST identifier nodes, and regex word-boundary fallback.
bool textReferencesIdentifier(const QString& source, const QString& identifier);

}  // namespace app::expr

