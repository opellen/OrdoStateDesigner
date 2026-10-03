#include "model/sim_agent.h"

#include <cmath>
#include <functional>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include "infra/expression.h"
#include "model/machine_doc.h"

namespace app {

namespace {

QVariant jsonValueToVariant(const QJsonValue& val) {
    if (val.isBool()) return val.toBool();
    if (val.isDouble()) {
        const double num = val.toDouble();
        if (std::isfinite(num) && num == std::trunc(num) && std::fabs(num) < 9.2e18) {
            return static_cast<qint64>(num);
        }
        return num;
    }
    if (val.isString()) return val.toString();
    if (val.isObject()) {
        QVariantMap map;
        const QJsonObject obj = val.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            map[it.key()] = jsonValueToVariant(it.value());
        }
        return map;
    }
    return val.toVariant();
}

QVariantMap defaultStructMap(const QString& structTypeName, const QVector<StructDefinition>& structDefs) {
    const StructDefinition* def = nullptr;
    for (const StructDefinition& sd : structDefs) {
        if (sd.name == structTypeName) {
            def = &sd;
            break;
        }
    }
    if (!def) {
        return {};
    }
    QVariantMap map;
    for (const StructField& field : def->fields) {
        switch (field.type) {
            case FieldType::Bool: {
                bool val = false;
                if (!field.initialValue.trimmed().isEmpty()) {
                    val = field.initialValue.trimmed().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
                }
                map[field.name] = val;
                break;
            }
            case FieldType::Int: {
                qint64 val = 0;
                if (!field.initialValue.trimmed().isEmpty()) {
                    bool ok = false;
                    val = field.initialValue.trimmed().toLongLong(&ok);
                }
                map[field.name] = val;
                break;
            }
            case FieldType::Double: {
                double val = 0.0;
                if (!field.initialValue.trimmed().isEmpty()) {
                    bool ok = false;
                    val = field.initialValue.trimmed().toDouble(&ok);
                }
                map[field.name] = val;
                break;
            }
            case FieldType::String:
                map[field.name] = field.initialValue;
                break;
            case FieldType::Custom:
                map[field.name] = defaultStructMap(field.customTypeName, structDefs);
                break;
        }
    }
    return map;
}

bool updateNestedMap(QVariantMap& map, const QStringList& segments, int index,
                     const std::function<bool(const QVariant&, QVariant&, QString&)>& leafUpdater,
                     QString& shown) {
    if (index >= segments.size()) {
        return false;
    }
    const QString& key = segments.at(index);
    if (index == segments.size() - 1) {
        QVariant stored;
        if (!leafUpdater(map.value(key), stored, shown)) {
            return false;
        }
        map[key] = stored;
        return true;
    }
    if (!map.contains(key)) {
        map[key] = QVariantMap{};
    }
    const QVariant sub = map.value(key);
    if (sub.typeId() != QMetaType::QVariantMap && !sub.canConvert<QVariantMap>()) {
        return false;
    }
    QVariantMap subMap = sub.toMap();
    if (!updateNestedMap(subMap, segments, index + 1, leafUpdater, shown)) {
        return false;
    }
    map[key] = subMap;
    return true;
}

}  // namespace

void SimulationAgent::armStateTimers(const MachineDocAgent& doc, quint64 stateId) {
    for (const Transition& transition : doc.machine().transitions) {
        if (transition.from != stateId) {
            continue;
        }
        const expr::TimeTrigger tt = expr::parseTimeTrigger(transition.event);
        const bool isTimeTrigger = tt.ok || (transition.event.isEmpty() && transition.delayMs > 0);
        if (isTimeTrigger) {
            const int duration = tt.ok ? tt.durationMs : transition.delayMs;
            const bool periodic = transition.periodic || (tt.ok && tt.isEvery());
            armedTimers_.push_back(ArmedTimer{
                .transitionId = transition.id,
                .remainingMs = duration,
                .durationMs = duration,
                .periodic = periodic,
                .ownerStateId = stateId,
            });
        }
    }
}

void SimulationAgent::disarmStateTimers(quint64 stateId) {
    std::erase_if(armedTimers_, [&](const ArmedTimer& timer) { return timer.ownerStateId == stateId; });
}

void SimulationAgent::armRootDelayedTransitions(const MachineDocAgent& doc) {
    std::erase_if(armedTimers_, [](const ArmedTimer& timer) { return timer.ownerStateId == 0; });
    for (const Transition& transition : doc.machine().transitions) {
        if (transition.from != 0) {
            continue;
        }
        const expr::TimeTrigger tt = expr::parseTimeTrigger(transition.event);
        const bool isTimeTrigger = tt.ok || (transition.event.isEmpty() && transition.delayMs > 0);
        if (isTimeTrigger) {
            const int duration = tt.ok ? tt.durationMs : transition.delayMs;
            const bool periodic = transition.periodic || (tt.ok && tt.isEvery());
            armedTimers_.push_back(ArmedTimer{
                .transitionId = transition.id,
                .remainingMs = duration,
                .durationMs = duration,
                .periodic = periodic,
                .ownerStateId = 0,
            });
        }
    }
}

void SimulationAgent::armStateInvocation(const MachineDocAgent& doc, quint64 stateId) {
    const State* state = doc.findState(stateId);
    if (!state) {
        return;
    }
    for (const Invocation& inv : state->effectiveInvocations()) {
        const QString invokeId = effectiveInvocationId(inv);
        if (invokeId.isEmpty()) {
            continue;
        }
        activeInvocations_.push_back(ActiveInvocation{.stateId = stateId, .invokeId = invokeId});
        appendTrace(QStringLiteral("invoke: ") + invokeId + QStringLiteral(" started"));
    }
}

void SimulationAgent::disarmStateInvocation(quint64 stateId) {
    for (auto it = activeInvocations_.begin(); it != activeInvocations_.end(); ) {
        if (it->stateId == stateId) {
            appendTrace(QStringLiteral("invoke: ") + it->invokeId + QStringLiteral(" cancelled"));
            it = activeInvocations_.erase(it);
        } else {
            ++it;
        }
    }
}

QSet<QString> SimulationAgent::liveInvocationIds() const {
    QSet<QString> ids;
    for (const ActiveInvocation& invocation : activeInvocations_) {
        ids.insert(invocation.invokeId);
    }
    return ids;
}

QVector<quint64> SimulationAgent::configurationSnapshot() const {
    QVector<quint64> snapshot;
    snapshot.reserve(static_cast<int>(configuration_.size()));
    for (quint64 id : configuration_) {
        snapshot.push_back(id);
    }
    return snapshot;
}

void SimulationAgent::appendTrace(const QString& line) {
    trace_.push_back(line);
    context().send(events::TraceAppended{.text = line});
}

QString SimulationAgent::transitionHeadline(const Transition& transition, const QString& destName) const {
    const QString label = transition.isAlways() ? QStringLiteral("(always)")
                        : transition.event.isEmpty() ? QStringLiteral("(after %1ms)").arg(transition.delayMs)
                        : transition.event;
    if (transition.to == 0 && destName.isEmpty()) {
        return label + QStringLiteral(" (internal)");
    }
    return label + QStringLiteral(" → ") + destName;
}

bool SimulationAgent::hookGuardPasses(const QString& guardName) const {
    if (guardName.isEmpty()) {
        return true;
    }
    const auto it = guardResults_.find(guardName);
    return it == guardResults_.end() ? true : it.value();
}

bool SimulationAgent::guardPasses(const QString& guardSource, quint64 transitionId,
                                   const InvocationPayload& payload) {
    if (guardSource.isEmpty() || expr::isBareIdentifier(guardSource)) {
        const bool result = hookGuardPasses(guardSource);
        lastGuardEvaluations_.push_back(GuardEvaluation{
            .transitionId = transitionId, .source = guardSource, .decided = true, .result = result});
        return result;
    }
    const auto refuse = [this, &guardSource, transitionId](const QString& reason) {
        appendTrace(QStringLiteral("guard: ") + guardSource + QStringLiteral(" → false (") + reason +
                    QStringLiteral(")"));
        lastGuardEvaluations_.push_back(GuardEvaluation{
            .transitionId = transitionId, .source = guardSource, .decided = false, .result = false});
        return false;
    };
    auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return refuse(QStringLiteral("no document"));
    }
    const expr::ParseResult parsed = expr::parse(guardSource);
    if (!parsed.ok) {
        return refuse(parsed.message);
    }
    expr::PayloadBinding binding = expr::PayloadBinding::none();
    if (const Transition* transition = doc->findTransition(transitionId)) {
        switch (invokePayloadKindForTransition(doc->machine(), *transition)) {
            case InvokePayloadKind::Output:
                if (const State* invokingState = doc->findState(transition->from)) {
                    ContextType outType = invokingState->invokeOutputType;
                    for (const Invocation& inv : invokingState->effectiveInvocations()) {
                        const QString effId = effectiveInvocationId(inv);
                        if (transition->event == QStringLiteral("done.invoke.") + effId) {
                            outType = inv.outputType;
                            break;
                        }
                    }
                    binding = expr::PayloadBinding::forOnDone(outType);
                }
                break;
            case InvokePayloadKind::Error:
                binding = expr::PayloadBinding::error();
                break;
            case InvokePayloadKind::None:
                if (!transition->payloadType.isEmpty()) {
                    binding = expr::PayloadBinding::forEvent(transition->payloadType, doc->machine().types);
                }
                break;
        }
    }
    const QVector<expr::TypeProblem> problems = expr::typeCheck(parsed.ast, doc->machine().context, binding, doc->machine().types);
    if (!problems.isEmpty()) {
        return refuse(problems.first().message);
    }
    QVariantMap values = contextValues_;
    if (!payload.name.isEmpty()) {
        values[payload.name] = payload.value;
    }
    bool ok = false;
    const expr::Value value = expr::evaluate(parsed.ast, values, &ok);
    if (!ok) {
        return refuse(QStringLiteral("no value in context"));
    }
    if (value.type != expr::ValueType::Bool) {
        return refuse(QStringLiteral("not a condition"));
    }
    lastGuardEvaluations_.push_back(GuardEvaluation{
        .transitionId = transitionId, .source = guardSource, .decided = true, .result = value.boolValue});
    return value.boolValue;
}

void SimulationAgent::runAction(const MachineDocAgent& doc, const QString& action, const InvocationPayload& payload) {
    const expr::SendToForm sendToForm = expr::parseSendToForm(action);
    if (sendToForm.ok) {
        appendTrace(QStringLiteral("sendTo: ") + sendToForm.target + QStringLiteral(" <- ") + sendToForm.event);
        return;
    }
    if (!sendToForm.message.isEmpty()) {
        appendTrace(QStringLiteral("sendTo skipped: ") + action + QStringLiteral(" (") + sendToForm.message + QStringLiteral(")"));
        return;
    }

    const expr::SendParentForm sendParentForm = expr::parseSendParentForm(action);
    if (sendParentForm.ok) {
        appendTrace(QStringLiteral("sendParent: ") + sendParentForm.event);
        return;
    }
    if (!sendParentForm.message.isEmpty()) {
        appendTrace(QStringLiteral("sendParent skipped: ") + action + QStringLiteral(" (") + sendParentForm.message + QStringLiteral(")"));
        return;
    }

    const expr::RaiseForm raiseForm = expr::parseRaiseForm(action);
    if (raiseForm.ok) {
        internalQueue_.push_back(raiseForm.event);
        appendTrace(QStringLiteral("raise: ") + raiseForm.event);
        return;
    }
    if (!raiseForm.message.isEmpty()) {
        appendTrace(QStringLiteral("raise skipped: ") + action + QStringLiteral(" (") + raiseForm.message + QStringLiteral(")"));
        return;
    }

    const expr::AssignForm form = expr::parseAssignForm(action);
    if (!form.ok && form.message.isEmpty()) {
        appendTrace(QStringLiteral("action: ") + action);
        return;
    }
    const auto skip = [this, &action](const QString& reason) {
        appendTrace(QStringLiteral("assign skipped: ") + action + QStringLiteral(" (") + reason + QStringLiteral(")"));
    };
    if (!form.ok) {
        skip(form.message);
        return;
    }
    const ContextVariable* target = nullptr;
    const QString rootName = form.rootTarget();
    for (const ContextVariable& variable : doc.machine().context) {
        if (variable.name == rootName) {
            target = &variable;
            break;
        }
    }
    if (target == nullptr) {
        skip(QStringLiteral("no context variable named '") + rootName + QStringLiteral("'"));
        return;
    }
    if (form.isMemberAssign() && target->type != ContextType::Object) {
        skip(QStringLiteral("context variable '") + rootName + QStringLiteral("' is not an Object"));
        return;
    }
    const expr::ParseResult parsed = expr::parse(form.valueSource);
    if (!parsed.ok) {
        skip(parsed.message);
        return;
    }
    QVariantMap values = contextValues_;
    if (!payload.name.isEmpty()) {
        values[payload.name] = payload.value;
    }
    bool ok = false;
    const expr::Value value = expr::evaluate(parsed.ast, values, &ok);
    if (!ok) {
        skip(QStringLiteral("value could not be evaluated"));
        return;
    }

    if (!form.isMemberAssign()) {
        QVariant stored;
        QString shown;
        switch (target->type) {
            case ContextType::Bool:
                if (value.type != expr::ValueType::Bool) {
                    skip(QStringLiteral("value is not a Bool"));
                    return;
                }
                stored = value.boolValue;
                shown = value.boolValue ? QStringLiteral("true") : QStringLiteral("false");
                break;
            case ContextType::Int: {
                if (!expr::isNumeric(value.type)) {
                    skip(QStringLiteral("value is not numeric"));
                    return;
                }
                const qint64 asInt = value.type == expr::ValueType::Int ? value.intValue
                                                                         : static_cast<qint64>(value.doubleValue);
                stored = asInt;
                shown = QString::number(asInt);
                break;
            }
            case ContextType::Double:
                if (!expr::isNumeric(value.type)) {
                    skip(QStringLiteral("value is not numeric"));
                    return;
                }
                stored = value.asDouble();
                shown = QString::number(value.asDouble());
                break;
            case ContextType::String:
                if (value.type != expr::ValueType::String) {
                    skip(QStringLiteral("value is not a String"));
                    return;
                }
                stored = value.stringValue;
                shown = QStringLiteral("'") + value.stringValue + QStringLiteral("'");
                break;
            case ContextType::Object:
                if (value.type != expr::ValueType::Object) {
                    skip(QStringLiteral("value is not an Object"));
                    return;
                }
                stored = value.objectValue;
                shown = QStringLiteral("[object Object]");
                break;
        }
        contextValues_[target->name] = stored;
        appendTrace(QStringLiteral("assign: ") + target->name + QStringLiteral(" = ") + shown);
        return;
    }

    // Member assign (e.g. user.age = 25 or user.profile.score = 99.5)
    const QStringList segments = form.memberPath().split(QLatin1Char('.'));
    QVariantMap rootMap = contextValues_.value(rootName).toMap();
    QString shown;
    auto leafUpdater = [&value](const QVariant& existingVal, QVariant& stored, QString& outShown) -> bool {
        if (existingVal.isValid()) {
            switch (existingVal.typeId()) {
                case QMetaType::Bool:
                    if (value.type != expr::ValueType::Bool) return false;
                    stored = value.boolValue;
                    outShown = value.boolValue ? QStringLiteral("true") : QStringLiteral("false");
                    return true;
                case QMetaType::Int:
                case QMetaType::UInt:
                case QMetaType::LongLong:
                case QMetaType::ULongLong: {
                    if (!expr::isNumeric(value.type)) return false;
                    const qint64 asInt = (value.type == expr::ValueType::Int) ? value.intValue
                                                                             : static_cast<qint64>(value.doubleValue);
                    stored = asInt;
                    outShown = QString::number(asInt);
                    return true;
                }
                case QMetaType::Float:
                case QMetaType::Double:
                    if (!expr::isNumeric(value.type)) return false;
                    stored = value.asDouble();
                    outShown = QString::number(value.asDouble());
                    return true;
                case QMetaType::QString:
                    if (value.type != expr::ValueType::String) return false;
                    stored = value.stringValue;
                    outShown = QStringLiteral("'") + value.stringValue + QStringLiteral("'");
                    return true;
                case QMetaType::QVariantMap:
                    if (value.type != expr::ValueType::Object) return false;
                    stored = value.objectValue;
                    outShown = QStringLiteral("[object Object]");
                    return true;
                default:
                    break;
            }
        }
        switch (value.type) {
            case expr::ValueType::Bool:
                stored = value.boolValue;
                outShown = value.boolValue ? QStringLiteral("true") : QStringLiteral("false");
                return true;
            case expr::ValueType::Int:
                stored = value.intValue;
                outShown = QString::number(value.intValue);
                return true;
            case expr::ValueType::Double:
                stored = value.doubleValue;
                outShown = QString::number(value.doubleValue);
                return true;
            case expr::ValueType::String:
                stored = value.stringValue;
                outShown = QStringLiteral("'") + value.stringValue + QStringLiteral("'");
                return true;
            case expr::ValueType::Object:
                stored = value.objectValue;
                outShown = QStringLiteral("[object Object]");
                return true;
        }
        return false;
    };

    if (!updateNestedMap(rootMap, segments, 0, leafUpdater, shown)) {
        skip(QStringLiteral("failed to update member property"));
        return;
    }
    contextValues_[rootName] = rootMap;
    appendTrace(QStringLiteral("assign: ") + form.target + QStringLiteral(" = ") + shown);
}

void SimulationAgent::seedContext(const MachineDocAgent& doc) {
    contextValues_.clear();
    for (const ContextVariable& variable : doc.machine().context) {
        const QString raw = variable.initialValue;
        const QString trimmed = raw.trimmed();
        bool parsedOk = true;
        switch (variable.type) {
            case ContextType::Bool: {
                const bool isTrue = trimmed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
                const bool isFalse = trimmed.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0;
                parsedOk = isTrue || isFalse;
                contextValues_[variable.name] = isTrue;
                break;
            }
            case ContextType::Int: {
                const qint64 parsed = trimmed.toLongLong(&parsedOk);
                contextValues_[variable.name] = parsedOk ? parsed : static_cast<qint64>(0);
                break;
            }
            case ContextType::Double: {
                const double parsed = trimmed.toDouble(&parsedOk);
                contextValues_[variable.name] = parsedOk ? parsed : 0.0;
                break;
            }
            case ContextType::String:
                contextValues_[variable.name] = raw;
                break;
            case ContextType::Object: {
                QVariantMap map;
                if (!variable.customTypeName.isEmpty()) {
                    map = defaultStructMap(variable.customTypeName, doc.machine().types);
                }
                if (trimmed.isEmpty()) {
                    contextValues_[variable.name] = map;
                    parsedOk = true;
                } else {
                    QJsonParseError err;
                    const QJsonDocument jsonDoc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
                    if (!jsonDoc.isNull() && jsonDoc.isObject()) {
                        const QJsonObject obj = jsonDoc.object();
                        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
                            map[it.key()] = jsonValueToVariant(it.value());
                        }
                        contextValues_[variable.name] = map;
                        parsedOk = true;
                    } else {
                        parsedOk = false;
                        contextValues_[variable.name] = map;
                    }
                }
                break;
            }
        }
        if (!parsedOk) {
            appendTrace(QStringLiteral("context: ") + variable.name + QStringLiteral(" = ") +
                        contextValues_.value(variable.name).toString() + QStringLiteral(" (initial value '") + raw +
                        QStringLiteral("' is not valid)"));
        }
    }
}

}  // namespace app
