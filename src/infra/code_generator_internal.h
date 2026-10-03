#pragma once

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <limits>

#include "infra/code_generator.h"
#include "infra/expression.h"
#include "model/machine.h"

namespace app {

constexpr const char* kIndent = "    ";
constexpr const char* kContextMember = "context_";

class NameCollector {
public:
    explicit NameCollector(bool capitalizeFirst) : capitalizeFirst_(capitalizeFirst) {}

    void add(const QString& raw) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty() || seenRaw_.contains(trimmed)) {
            return;
        }
        seenRaw_.insert(trimmed);
        rawOrder_.push_back(trimmed);
        idByRaw_.insert(trimmed, sanitizeIdentifier(trimmed, capitalizeFirst_));
    }

    void addCustom(const QString& raw, const QString& customId) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty() || seenRaw_.contains(trimmed)) {
            return;
        }
        seenRaw_.insert(trimmed);
        rawOrder_.push_back(trimmed);
        idByRaw_.insert(trimmed, customId);
    }

    const QStringList& rawOrder() const { return rawOrder_; }
    QString idOf(const QString& raw) const { return idByRaw_.value(raw.trimmed()); }
    bool isEmpty() const { return rawOrder_.isEmpty(); }

private:
    bool capitalizeFirst_;
    QSet<QString> seenRaw_;
    QStringList rawOrder_;
    QHash<QString, QString> idByRaw_;
};


enum class HierHistoryRedirect { None, OneLevel, AllLevels };

struct HierState {
    int parentIndex = -1;        // -1 = direct child of the machine root
    int depth = 0;                // parentId-chain length (0 = top-level)
    int initialChildIndex = -1;   // -1 = no children (atomic) or unset
    bool isParallel = false;
};


struct HierTransition {
    int sourceIndex = -1;          // -1 = root (declared from == 0)
    int declaredTargetIndex = -1;  // -1 = targetless; reported as `next` as declared, never the resolved leaf
    int targetIndex = -1;          // entry target: declaredTargetIndex, or a History target's parent
    int lccaIndex = -1;            // -1 = root domain; meaningless when targetless
    HierHistoryRedirect historyPolicy = HierHistoryRedirect::None;
    bool reenter = false;
    bool always = false;
    bool isMultiTarget = false;
    QVector<int> targetIndices;
};


struct GenModel {
    QString machineNs;      // lower_snake, e.g. "login_flow"
    QString machinePascal;  // PascalCase, e.g. "LoginFlow"
    QString nsJoined;       // full C++ namespace, e.g. "app::generated::login_flow"

    QHash<quint64, QString> stateIdentById;
    QString defaultStateIdent;  // Machine::initialStateId's state, else the first state, else empty

    QStringList eventOrder;                                  // raw event text, first-seen (document) order
    QHash<QString, QString> eventStructNameByRaw;             // raw -> "<Base>Requested"
    // raw -> "<base>Requested", the core method firing that event. Same words
    // as the struct name, so it adds no new collision domain.
    QHash<QString, QString> eventMethodByRaw;
    QHash<QString, QVector<const Transition*>> eventTransitions;  // raw -> transitions carrying it, document order

    NameCollector guards{false};
    NameCollector actions{false};

    // ---- extended state ----------------------------------------------------
    // `context` is Machine::context in document order, which is the emitted
    // struct's member order. hasContext == false emits no context code at all.
    QVector<ContextVariable> context;
    bool hasContext = false;
    bool hasStringContext = false;  // a String member means <string> in _hooks.h, and only then

    bool hasScheduler = false;
    // Source state id -> its delayed (blank event, delayMs > 0) transitions,
    // document order.
    QHash<quint64, QVector<const Transition*>> delayedBySource;

    bool hasAlways = false;
    QVector<const Transition*> alwaysTransitions;

    bool hasRaise = false;
    QHash<QString, QString> eventEnumIdentByRaw;

    // ---- invoke ------------------------------------------------------------
    // hasInvoke == false emits no invoke code. Keyed by the EFFECTIVE invoke
    // id, so states naming the same service share one hook method and one
    // std::stop_source.
    bool hasInvoke = false;
    QHash<quint64, QString> invokeEffectiveIdByStateId;    // invoking state id -> its primary effective invoke id
    QHash<quint64, QStringList> invokeEffectiveIdsByStateId; // invoking state id -> all effective invoke ids
    QStringList invokeIdOrder;                             // distinct effective ids, first-seen (document) order
    QHash<QString, ContextType> invokeOutputTypeByEffectiveId;  // effective id -> its declared onDone output type
    // Invoking state id -> its onDone/onError transitions, document order. Kept
    // out of eventOrder: these rows are never public events and fire only from
    // the invoke completion callbacks.
    QHash<quint64, QVector<const Transition*>> onDoneByState;
    QHash<quint64, QVector<const Transition*>> onErrorByState;

    bool hasMultiTarget = false;

    // ---- struct types & event payloads
    bool hasTypes = false;
    QHash<QString, QString> eventPayloadTypeByRaw;

    // ---- hierarchical-only, but always populated ---------------------------
    QHash<quint64, int> stateIndexById;         // state id -> document-order index
    QVector<HierState> hierStates;              // parallel to machine.states
    QVector<HierTransition> hierTransitions;    // parallel to machine.transitions
    int initialStateIndex = -1;                 // -1 only for an empty machine.states
};


// Common formatting & model helpers
QString bannerLines(const QString& oneLineDescription);
QString openNamespace(const QString& joined);
QString closeNamespace(const QString& joined);
QString contextTypeSpelling(ContextType type);
QString cppPayloadTypeSpelling(const QString& payloadType);
QString eventPayloadCppTypeForTransition(const Machine& machine, const Transition& transition);
QString cppDoubleLiteral(double value);
QString contextInitialiser(const ContextVariable& variable);
void emitNestedStruct(QString& out, const QJsonObject& obj, const QString& structTypeName, const QString& memberName, int indentLevel);
const State* findStateById(const Machine& machine, quint64 id);
quint64 parentIdOf(const Machine& machine, quint64 stateId);
bool isAncestorId(const Machine& machine, quint64 ancestorId, quint64 descendantId);
quint64 lccaId(const Machine& machine, quint64 sourceId, quint64 targetId);
quint64 lccaId(const Machine& machine, quint64 sourceId, const QList<quint64>& targetIds);
int depthOfId(const Machine& machine, quint64 stateId);
QString invokeHookMethodName(const QString& effectiveId);
QString invokeStopSourceMember(const QString& effectiveId);
QString extraCtorParams(const GenModel& model);
QString extraCtorInit(const GenModel& model);
QString extraArgForward(const GenModel& model);
QString extraStdRefForward(const GenModel& model);
QString extraRefComment(const GenModel& model);
bool guardIsHook(const GenModel& model, const QString& guard, bool payloadInScope = false);
bool actionIsHook(const GenModel& model, const QString& action, bool payloadInScope = false);
QString eventEnumIdent(const GenModel& model, const QString& rawEvent);
QString guardCondition(const GenModel& model, const QString& guard, bool negated, const QSet<QString>& bareIdentifiers = {});
QString actionStatement(const GenModel& model, const QString& action, const QSet<QString>& bareIdentifiers = {});
void appendAction(QString& text, const GenModel& model, const QString& indent, const QString& action, const QSet<QString>& bareIdentifiers = {});
QString contextAccessorLines();
QString contextMemberLine();
GenModel buildModel(const Machine& machine, const QString& rootNamespace);
QString stateIdent(const GenModel& model, quint64 stateId);

// Builders for auxiliary/shell files
GeneratedFile buildTypesFile(const Machine& machine, const GenModel& model);
GeneratedFile buildStateFile(const Machine& machine, const GenModel& model, bool hierarchical);
GeneratedFile buildEventsFile(const GenModel& model);
GeneratedFile buildHooksFile(const GenModel& model, bool hierarchical);
GeneratedFile buildAgentFile(const GenModel& model, bool hierarchical);
GeneratedFile buildCommandsFile(const GenModel& model, bool hierarchical);
GeneratedFile buildBootstrapFile(const GenModel& model, bool hierarchical);
QVector<GeneratedFile> buildDomainStubFiles(const Machine& machine, const GenModel& model);

// Builders for Core files
GeneratedFile buildCoreFile(const Machine& machine, const GenModel& model);
GeneratedFile buildHierarchicalCoreFile(const Machine& machine, const GenModel& model);

}  // namespace app
