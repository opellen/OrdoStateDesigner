// Value/PayloadBinding and the action/label form parsers. Tokenizer and parser:
// expression_parser.cpp; typeCheck: expression_typecheck.cpp; evaluate:
// expression_eval.cpp; print/emitCpp: expression_emit.cpp.

#include "infra/expression.h"

#include <QChar>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace app::expr {

// ---- shared helpers ------------------------------------------------------

ValueType fromContextType(ContextType type) {
    switch (type) {
        case ContextType::Bool: return ValueType::Bool;
        case ContextType::Int: return ValueType::Int;
        case ContextType::Double: return ValueType::Double;
        case ContextType::String: return ValueType::String;
        case ContextType::Object: return ValueType::Object;
    }
    return ValueType::Bool;
}

// ---- Value ---------------------------------------------------------------

Value Value::fromBool(bool value) {
    Value result;
    result.type = ValueType::Bool;
    result.boolValue = value;
    return result;
}

Value Value::fromInt(qint64 value) {
    Value result;
    result.type = ValueType::Int;
    result.intValue = value;
    return result;
}

Value Value::fromDouble(double value) {
    Value result;
    result.type = ValueType::Double;
    result.doubleValue = value;
    return result;
}

Value Value::fromString(const QString& value) {
    Value result;
    result.type = ValueType::String;
    result.stringValue = value;
    return result;
}

Value Value::fromObject(const QVariantMap& value) {
    Value result;
    result.type = ValueType::Object;
    result.objectValue = value;
    return result;
}

double Value::asDouble() const {
    return (type == ValueType::Int) ? static_cast<double>(intValue) : doubleValue;
}

bool isNumeric(ValueType type) {
    return type == ValueType::Int || type == ValueType::Double;
}

// ---- PayloadBinding --------------------------------------------------------
// The single home of the payload name-to-type pairing; callers never re-decide it.

PayloadBinding PayloadBinding::none() {
    return PayloadBinding();
}

PayloadBinding PayloadBinding::forOnDone(ContextType type) {
    PayloadBinding binding;
    binding.kind = PayloadKind::Output;
    binding.name = QStringLiteral("output");
    binding.type = fromContextType(type);
    return binding;
}

PayloadBinding PayloadBinding::error() {
    PayloadBinding binding;
    binding.kind = PayloadKind::Error;
    binding.name = QStringLiteral("error");
    binding.type = ValueType::String;
    return binding;
}

PayloadBinding PayloadBinding::forEvent(const QString& payloadType, const QVector<StructDefinition>& structDefs) {
    if (payloadType.trimmed().isEmpty()) {
        return none();
    }
    PayloadBinding binding;
    binding.kind = PayloadKind::Event;
    binding.name = QStringLiteral("event");
    if (const auto scalar = builtInPayloadScalar(payloadType)) {
        binding.type = fromContextType(*scalar);
    } else {
        const QString trimmed = payloadType.trimmed();
        binding.type = ValueType::Object;
        binding.structTypeName = trimmed;
        for (const StructDefinition& def : structDefs) {
            if (def.name == trimmed) {
                binding.structDef = &def;
                break;
            }
        }
    }
    return binding;
}

// ---- Action and Label Form Parsers ---------------------------------------

AssignForm parseAssignForm(const QString& source) {
    AssignForm result;

    // A raw character scan, not tokenize(): the tokenizer rejects a lone '='.
    // `inString` mirrors its single-quote rule so `role = 'a=b'` splits once.
    const int size = static_cast<int>(source.size());
    int splitPos = -1;
    int secondPos = -1;
    bool inString = false;
    for (int i = 0; i < size; ++i) {
        const QChar c = source.at(i);
        if (inString) {
            if (c == QLatin1Char('\'')) {
                inString = false;
            }
            continue;
        }
        if (c == QLatin1Char('\'')) {
            inString = true;
            continue;
        }
        if (c != QLatin1Char('=')) {
            continue;
        }
        // '==': consume both characters; neither is a split point.
        if (i + 1 < size && source.at(i + 1) == QLatin1Char('=')) {
            ++i;
            continue;
        }
        // Second character of '!=', '<=' or '>='.
        if (i > 0 && (source.at(i - 1) == QLatin1Char('!') || source.at(i - 1) == QLatin1Char('<') ||
                      source.at(i - 1) == QLatin1Char('>'))) {
            continue;
        }
        if (splitPos == -1) {
            splitPos = i;
        } else if (secondPos == -1) {
            secondPos = i;
        }
    }

    if (splitPos == -1) {
        // No top-level '=': a plain action hook, signalled by a blank message.
        return result;
    }
    if (secondPos != -1) {
        // `a = b = c`: there are no nested assignments.
        result.message =
            QStringLiteral("an assign may have only one top-level '=' -- grammar v1 has no nested assignment");
        result.position = secondPos;
        return result;
    }

    const QString left = source.left(splitPos);
    const QString right = source.mid(splitPos + 1);

    const auto isValidAssignTarget = [](const QString& targetStr) -> bool {
        const QString trimmed = targetStr.trimmed();
        if (trimmed.isEmpty()) return false;
        const QStringList parts = trimmed.split(QLatin1Char('.'));
        for (const QString& part : parts) {
            if (!isBareIdentifier(part)) {
                return false;
            }
        }
        return true;
    };

    if (!isValidAssignTarget(left)) {
        if (left.contains(QLatin1Char('.'))) {
            result.message =
                QStringLiteral("the left side of '=' must be an identifier or member path (e.g. 'user.field'), got '%1'").arg(left.trimmed());
        } else {
            result.message =
                QStringLiteral("the left side of '=' must be exactly one identifier, got '%1'").arg(left.trimmed());
        }
        result.position = 0;
        return result;
    }
    if (right.trimmed().isEmpty()) {
        result.message = QStringLiteral("the right side of '=' is blank");
        result.position = splitPos + 1;
        return result;
    }

    result.ok = true;
    result.target = left.trimmed();
    result.valueSource = right.trimmed();
    return result;
}

RaiseForm parseRaiseForm(const QString& source) {
    RaiseForm result;
    const QString trimmed = source.trimmed();

    if (!trimmed.startsWith(QStringLiteral("raise"), Qt::CaseInsensitive)) {
        return result;
    }

    const QString afterRaise = trimmed.mid(5);
    const QString afterRaiseTrimmed = afterRaise.trimmed();
    if (!afterRaiseTrimmed.startsWith(QLatin1Char('('))) {
        if (afterRaiseTrimmed.isEmpty()) {
            result.message = QStringLiteral("raise requires an event in parentheses: raise(Event)");
            result.position = 5;
            return result;
        }
        if (!afterRaise.isEmpty()) {
            const QChar firstAfter = afterRaise.at(0);
            if (firstAfter.isLetterOrNumber() || firstAfter == QLatin1Char('_')) {
                return result;
            }
        }
        result.message = QStringLiteral("expected '(' after 'raise'");
        result.position = 5;
        return result;
    }

    const int openParen = trimmed.indexOf(QLatin1Char('('));
    if (!trimmed.endsWith(QLatin1Char(')'))) {
        result.message = QStringLiteral("missing closing ')' in raise action");
        result.position = static_cast<int>(trimmed.size());
        return result;
    }

    const int closeParen = trimmed.lastIndexOf(QLatin1Char(')'));
    const QString inner = trimmed.mid(openParen + 1, closeParen - openParen - 1).trimmed();
    if (inner.isEmpty()) {
        result.message = QStringLiteral("raise requires an event name inside parentheses");
        result.position = openParen + 1;
        return result;
    }

    for (int i = 0; i < inner.size(); ++i) {
        const QChar c = inner.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('.')) {
            result.message = QStringLiteral("invalid character '%1' in raised event name '%2'").arg(c, inner);
            result.position = openParen + 1 + i;
            return result;
        }
    }

    result.ok = true;
    result.event = inner;
    return result;
}

SendToForm parseSendToForm(const QString& source) {
    SendToForm result;
    const QString trimmed = source.trimmed();

    if (!trimmed.startsWith(QStringLiteral("sendTo"), Qt::CaseInsensitive)) {
        return result;
    }

    const QString afterPrefix = trimmed.mid(6);
    if (!afterPrefix.isEmpty()) {
        const QChar firstAfter = afterPrefix.at(0);
        if (firstAfter.isLetterOrNumber() || firstAfter == QLatin1Char('_')) {
            return result;  // e.g. sendTokens() -> hook
        }
    }

    const QString afterTrimmed = afterPrefix.trimmed();
    if (!afterTrimmed.startsWith(QLatin1Char('('))) {
        if (afterTrimmed.isEmpty()) {
            result.message = QStringLiteral("sendTo requires arguments in parentheses: sendTo(actor, Event)");
            result.position = 6;
            return result;
        }
        result.message = QStringLiteral("expected '(' after 'sendTo'");
        result.position = 6;
        return result;
    }

    const int openParen = trimmed.indexOf(QLatin1Char('('));
    if (!trimmed.endsWith(QLatin1Char(')'))) {
        result.message = QStringLiteral("missing closing ')' in sendTo action");
        result.position = static_cast<int>(trimmed.size());
        return result;
    }

    const int closeParen = trimmed.lastIndexOf(QLatin1Char(')'));
    const QString inner = trimmed.mid(openParen + 1, closeParen - openParen - 1).trimmed();
    if (inner.isEmpty()) {
        result.message = QStringLiteral("sendTo requires target actor and event name inside parentheses");
        result.position = openParen + 1;
        return result;
    }

    const int commaIdx = inner.indexOf(QLatin1Char(','));
    if (commaIdx < 0) {
        result.message = QStringLiteral("sendTo requires two arguments separated by comma: sendTo(actor, Event)");
        result.position = openParen + 1;
        return result;
    }

    const QString targetStr = inner.left(commaIdx).trimmed();
    const QString eventStr = inner.mid(commaIdx + 1).trimmed();
    if (targetStr.isEmpty() || eventStr.isEmpty()) {
        result.message = QStringLiteral("sendTo requires both a target actor and an event name");
        result.position = openParen + 1;
        return result;
    }

    for (int i = 0; i < targetStr.size(); ++i) {
        const QChar c = targetStr.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-') && c != QLatin1Char('.')) {
            result.message = QStringLiteral("invalid character '%1' in sendTo target '%2'").arg(c, targetStr);
            result.position = openParen + 1 + i;
            return result;
        }
    }

    for (int i = 0; i < eventStr.size(); ++i) {
        const QChar c = eventStr.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('.')) {
            result.message = QStringLiteral("invalid character '%1' in sendTo event name '%2'").arg(c, eventStr);
            result.position = openParen + 1 + commaIdx + 1 + i;
            return result;
        }
    }

    result.ok = true;
    result.target = targetStr;
    result.event = eventStr;
    return result;
}

SendParentForm parseSendParentForm(const QString& source) {
    SendParentForm result;
    const QString trimmed = source.trimmed();

    if (!trimmed.startsWith(QStringLiteral("sendParent"), Qt::CaseInsensitive)) {
        return result;
    }

    const QString afterPrefix = trimmed.mid(10);
    if (!afterPrefix.isEmpty()) {
        const QChar firstAfter = afterPrefix.at(0);
        if (firstAfter.isLetterOrNumber() || firstAfter == QLatin1Char('_')) {
            return result;  // e.g. sendParental() -> hook
        }
    }

    const QString afterTrimmed = afterPrefix.trimmed();
    if (!afterTrimmed.startsWith(QLatin1Char('('))) {
        if (afterTrimmed.isEmpty()) {
            result.message = QStringLiteral("sendParent requires an event in parentheses: sendParent(Event)");
            result.position = 10;
            return result;
        }
        result.message = QStringLiteral("expected '(' after 'sendParent'");
        result.position = 10;
        return result;
    }

    const int openParen = trimmed.indexOf(QLatin1Char('('));
    if (!trimmed.endsWith(QLatin1Char(')'))) {
        result.message = QStringLiteral("missing closing ')' in sendParent action");
        result.position = static_cast<int>(trimmed.size());
        return result;
    }

    const int closeParen = trimmed.lastIndexOf(QLatin1Char(')'));
    const QString inner = trimmed.mid(openParen + 1, closeParen - openParen - 1).trimmed();
    if (inner.isEmpty()) {
        result.message = QStringLiteral("sendParent requires an event name inside parentheses");
        result.position = openParen + 1;
        return result;
    }

    for (int i = 0; i < inner.size(); ++i) {
        const QChar c = inner.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('.')) {
            result.message = QStringLiteral("invalid character '%1' in sendParent event name '%2'").arg(c, inner);
            result.position = openParen + 1 + i;
            return result;
        }
    }

    result.ok = true;
    result.event = inner;
    return result;
}

TimeTrigger parseTimeTrigger(const QString& source) {
    TimeTrigger result;
    QString trimmed = source.trimmed();
    if (trimmed.isEmpty()) {
        return result;
    }

    // Strip optional surrounding parentheses, e.g. "(after 500 ms)" or "(every 1s)"
    if (trimmed.startsWith(QLatin1Char('(')) && trimmed.endsWith(QLatin1Char(')'))) {
        trimmed = trimmed.mid(1, trimmed.size() - 2).trimmed();
    }

    TimeTriggerKind kind = TimeTriggerKind::None;
    QString remainder;
    if (trimmed.startsWith(QStringLiteral("after"), Qt::CaseInsensitive)) {
        kind = TimeTriggerKind::After;
        remainder = trimmed.mid(5).trimmed();
    } else if (trimmed.startsWith(QStringLiteral("every"), Qt::CaseInsensitive)) {
        kind = TimeTriggerKind::Every;
        remainder = trimmed.mid(5).trimmed();
    } else {
        return result;
    }

    // Must have whitespace or delimiter after 'after' / 'every' in original text
    // (e.g. "aftermath" or "everyday" should NOT be parsed as time triggers!)
    if (trimmed.size() > 5 && trimmed.at(5).isLetterOrNumber()) {
        return result;
    }

    if (remainder.isEmpty()) {
        result.kind = kind;
        result.ok = false;
        result.message = (kind == TimeTriggerKind::After)
                             ? QStringLiteral("after requires a duration (e.g. 'after 10s', 'after 500ms')")
                             : QStringLiteral("every requires an interval (e.g. 'every 500ms', 'every 1s')");
        result.position = 5;
        return result;
    }

    result.kind = kind;
    result.rawDuration = remainder;

    // Parse duration: numeric part + unit
    // Examples: "10s", "500ms", "250ms", "1.5s", "0.5s", "100", "2m", "1min", "500 ms"
    int numEnd = 0;
    while (numEnd < remainder.size() && (remainder.at(numEnd).isDigit() || remainder.at(numEnd) == QLatin1Char('.'))) {
        ++numEnd;
    }

    if (numEnd == 0) {
        result.ok = false;
        result.message = QStringLiteral("invalid time value in duration '%1'").arg(remainder);
        result.position = 5;
        return result;
    }

    bool parseOk = false;
    const double val = remainder.left(numEnd).toDouble(&parseOk);
    if (!parseOk || val < 0.0) {
        result.ok = false;
        result.message = QStringLiteral("cannot parse numeric duration '%1'").arg(remainder.left(numEnd));
        result.position = 5;
        return result;
    }

    const QString unit = remainder.mid(numEnd).trimmed().toLower();
    double multiplier = 1.0;  // default is ms
    if (unit.isEmpty() || unit == QStringLiteral("ms")) {
        multiplier = 1.0;
    } else if (unit == QStringLiteral("s") || unit == QStringLiteral("sec")) {
        multiplier = 1000.0;
    } else if (unit == QStringLiteral("m") || unit == QStringLiteral("min")) {
        multiplier = 60000.0;
    } else if (unit == QStringLiteral("us")) {
        multiplier = 0.001;
    } else {
        result.ok = false;
        result.message = QStringLiteral("unknown time unit '%1' (expected ms, s, m, min)").arg(unit);
        result.position = 5 + numEnd;
        return result;
    }

    const int computedMs = static_cast<int>(std::round(val * multiplier));
    if (computedMs <= 0) {
        result.ok = false;
        result.message = QStringLiteral("time trigger duration must be greater than 0 ms");
        result.position = 5;
        return result;
    }

    result.durationMs = computedMs;
    result.ok = true;
    return result;
}

TransitionLabelForm parseTransitionLabel(const QString& source) {
    TransitionLabelForm result;
    const QString trimmed = source.trimmed();
    if (trimmed.isEmpty()) {
        return result;
    }

    // Check for guard enclosed in '[' ... ']'
    const int bracketOpen = trimmed.indexOf(QLatin1Char('['));
    const int bracketClose = (bracketOpen >= 0) ? trimmed.indexOf(QLatin1Char(']'), bracketOpen + 1) : -1;

    QString triggerPart;
    QString guardPart;
    QString actionPart;

    if (bracketOpen >= 0 && bracketClose > bracketOpen) {
        triggerPart = trimmed.left(bracketOpen).trimmed();
        guardPart = trimmed.mid(bracketOpen + 1, bracketClose - bracketOpen - 1).trimmed();
        const QString afterGuard = trimmed.mid(bracketClose + 1).trimmed();
        if (afterGuard.startsWith(QLatin1Char('/'))) {
            actionPart = afterGuard.mid(1).trimmed();
        }
    } else {
        // No brackets. Check for '/'
        const int slashIdx = trimmed.indexOf(QLatin1Char('/'));
        if (slashIdx >= 0) {
            triggerPart = trimmed.left(slashIdx).trimmed();
            actionPart = trimmed.mid(slashIdx + 1).trimmed();
        } else {
            triggerPart = trimmed;
        }
    }

    if (triggerPart.compare(QStringLiteral("always"), Qt::CaseInsensitive) == 0) {
        result.always = true;
        result.event = QString();
        result.hasEvent = true;
    } else {
        result.event = triggerPart;
        result.hasEvent = !triggerPart.isEmpty();
        if (result.hasEvent) {
            result.timeTrigger = parseTimeTrigger(triggerPart);
        }
    }

    if (!guardPart.isEmpty()) {
        result.guard = guardPart;
        result.hasGuard = true;
    }
    if (!actionPart.isEmpty()) {
        result.action = actionPart;
        result.hasAction = true;
    }

    return result;
}

// ---- Source Rewriting and References -------------------------------------

QString renameIdentifierInSource(const QString& source, const QString& before, const QString& after) {
    if (before.isEmpty() || before == after) {
        return source;
    }
    // A hook name is not a context reference; a context rename leaves it alone.
    if (isBareIdentifier(source)) {
        return source;
    }
    const ParseResult parsed = parse(source);
    if (!parsed.ok) {
        // Leave half-typed text untouched; the validator flags it.
        return source;
    }

    QVector<int> offsets;
    for (const Node& node : parsed.ast.nodes) {
        // Whole Identifier tokens only; string literals are data.
        if (node.kind == NodeKind::Identifier && node.text == before) {
            offsets.push_back(node.sourceOffset);
        }
    }
    // Right-to-left, so each splice leaves the pending offsets valid.
    std::sort(offsets.begin(), offsets.end(), std::greater<int>());
    QString result = source;
    const int length = static_cast<int>(before.size());
    for (const int offset : offsets) {
        result.replace(offset, length, after);
    }
    return result;
}

bool textReferencesIdentifier(const QString& source, const QString& identifier) {
    if (source.isEmpty() || identifier.isEmpty()) {
        return false;
    }

    const AssignForm assign = parseAssignForm(source);
    if (assign.ok) {
        if (assign.target == identifier || assign.rootTarget() == identifier) {
            return true;
        }
        return textReferencesIdentifier(assign.valueSource, identifier);
    }

    const ParseResult parsed = parse(source);
    if (parsed.ok) {
        for (const Node& node : parsed.ast.nodes) {
            if (node.kind == NodeKind::Identifier && node.text == identifier) {
                return true;
            }
        }
        return false;
    }

    const QRegularExpression regex(QStringLiteral("\\b") + QRegularExpression::escape(identifier) + QStringLiteral("\\b"));
    return source.contains(regex);
}

}  // namespace app::expr
