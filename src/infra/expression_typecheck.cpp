#include "infra/expression.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <optional>

namespace app::expr {

namespace {

ValueType fromFieldType(FieldType type) {
    switch (type) {
        case FieldType::Bool: return ValueType::Bool;
        case FieldType::Int: return ValueType::Int;
        case FieldType::Double: return ValueType::Double;
        case FieldType::String: return ValueType::String;
        case FieldType::Custom: return ValueType::Object;
    }
    return ValueType::Int;
}

std::optional<ValueType> checkStructMemberPath(const QString& rootType,
                                               const QStringList& properties,
                                               const QVector<StructDefinition>& structDefs,
                                               int sourceOffset,
                                               QVector<TypeProblem>& problems) {
    if (properties.isEmpty()) {
        return ValueType::Object;
    }
    const StructDefinition* currentStruct = nullptr;
    for (const StructDefinition& sd : structDefs) {
        if (sd.name == rootType) {
            currentStruct = &sd;
            break;
        }
    }
    if (!currentStruct) {
        problems.push_back({QStringLiteral("unknown struct type '%1'").arg(rootType), sourceOffset});
        return std::nullopt;
    }
    for (int i = 0; i < properties.size(); ++i) {
        const QString& prop = properties.at(i);
        const StructField* foundField = nullptr;
        for (const StructField& field : currentStruct->fields) {
            if (field.name == prop) {
                foundField = &field;
                break;
            }
        }
        if (!foundField) {
            problems.push_back({QStringLiteral("unknown field '%1' on struct '%2'").arg(prop, currentStruct->name),
                                sourceOffset});
            return std::nullopt;
        }
        if (i == properties.size() - 1) {
            return fromFieldType(foundField->type);
        }
        if (foundField->type != FieldType::Custom) {
            problems.push_back({QStringLiteral("field '%1' of struct '%2' is not a nested struct")
                                    .arg(prop, currentStruct->name),
                                sourceOffset});
            return std::nullopt;
        }
        const StructDefinition* nextStruct = nullptr;
        for (const StructDefinition& sd : structDefs) {
            if (sd.name == foundField->customTypeName) {
                nextStruct = &sd;
                break;
            }
        }
        if (!nextStruct) {
            problems.push_back({QStringLiteral("unknown struct type '%1' for field '%2'")
                                    .arg(foundField->customTypeName, prop),
                                sourceOffset});
            return std::nullopt;
        }
        currentStruct = nextStruct;
    }
    return std::nullopt;
}

QString typeName(ValueType type) {
    switch (type) {
        case ValueType::Bool: return QStringLiteral("Bool");
        case ValueType::Int: return QStringLiteral("Int");
        case ValueType::Double: return QStringLiteral("Double");
        case ValueType::String: return QStringLiteral("String");
        case ValueType::Object: return QStringLiteral("Object");
    }
    return QStringLiteral("Bool");
}

QString operatorText(NodeKind kind) {
    switch (kind) {
        case NodeKind::Not: return QStringLiteral("!");
        case NodeKind::Negate: return QStringLiteral("-");
        case NodeKind::And: return QStringLiteral("&&");
        case NodeKind::Or: return QStringLiteral("||");
        case NodeKind::Equal: return QStringLiteral("==");
        case NodeKind::NotEqual: return QStringLiteral("!=");
        case NodeKind::Less: return QStringLiteral("<");
        case NodeKind::LessEqual: return QStringLiteral("<=");
        case NodeKind::Greater: return QStringLiteral(">");
        case NodeKind::GreaterEqual: return QStringLiteral(">=");
        case NodeKind::Add: return QStringLiteral("+");
        case NodeKind::Subtract: return QStringLiteral("-");
        case NodeKind::Multiply: return QStringLiteral("*");
        case NodeKind::Divide: return QStringLiteral("/");
        case NodeKind::Modulo: return QStringLiteral("%");
        default: break;
    }
    return QString();
}

bool isComparison(NodeKind kind) {
    switch (kind) {
        case NodeKind::Equal:
        case NodeKind::NotEqual:
        case NodeKind::Less:
        case NodeKind::LessEqual:
        case NodeKind::Greater:
        case NodeKind::GreaterEqual:
            return true;
        default:
            return false;
    }
}

bool isEquality(NodeKind kind) {
    return kind == NodeKind::Equal || kind == NodeKind::NotEqual;
}

// ---- typeCheck -----------------------------------------------------------

struct MemberChain {
    bool valid = false;
    QString rootIdentifier;
    QStringList propertyChain;
};

MemberChain extractMemberChain(const Ast& ast, int index) {
    MemberChain result;
    QStringList rpath;
    int curr = index;
    while (curr >= 0 && curr < static_cast<int>(ast.nodes.size())) {
        const Node& n = ast.nodes.at(static_cast<size_t>(curr));
        if (n.kind == NodeKind::MemberAccess) {
            rpath.prepend(n.text);
            curr = n.lhs;
        } else if (n.kind == NodeKind::Identifier) {
            result.rootIdentifier = n.text;
            result.propertyChain = rpath;
            result.valid = true;
            return result;
        } else {
            return result;
        }
    }
    return result;
}

// Returns nullopt when the subtree is ill-typed; a parent of a nullopt child
// adds no second problem, so one bad leaf yields one marker. Identifiers
// resolve against `payload` before `schema`.
std::optional<ValueType> checkNode(const Ast& ast, int index, const QVector<ContextVariable>& schema,
                                   const PayloadBinding& payload, QVector<TypeProblem>& problems,
                                   const QVector<StructDefinition>& structDefs) {
    const Node& node = ast.nodes.at(static_cast<size_t>(index));
    switch (node.kind) {
        case NodeKind::BoolLiteral: return ValueType::Bool;
        case NodeKind::IntLiteral: return ValueType::Int;
        case NodeKind::DoubleLiteral: return ValueType::Double;
        case NodeKind::StringLiteral: return ValueType::String;
        case NodeKind::Identifier: {
            if (!payload.name.isEmpty() && node.text == payload.name) {
                return payload.type;
            }
            for (const ContextVariable& variable : schema) {
                if (variable.name == node.text) {
                    return fromContextType(variable.type);
                }
            }
            problems.push_back({QStringLiteral("unknown identifier '%1'").arg(node.text), node.sourceOffset});
            return std::nullopt;
        }
        case NodeKind::Not: {
            const std::optional<ValueType> operand = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
            if (!operand) {
                return std::nullopt;
            }
            if (*operand != ValueType::Bool) {
                problems.push_back({QStringLiteral("'!' expects Bool, got %1").arg(typeName(*operand)),
                                    node.sourceOffset});
                return std::nullopt;
            }
            return ValueType::Bool;
        }
        case NodeKind::Negate: {
            const std::optional<ValueType> operand = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
            if (!operand) {
                return std::nullopt;
            }
            if (!isNumeric(*operand)) {
                problems.push_back({QStringLiteral("unary '-' expects Int or Double, got %1").arg(typeName(*operand)),
                                    node.sourceOffset});
                return std::nullopt;
            }
            return *operand;
        }
        case NodeKind::And:
        case NodeKind::Or: {
            // Both sides are checked (no short-circuit) to report every finding.
            const std::optional<ValueType> lhs = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
            const std::optional<ValueType> rhs = checkNode(ast, node.rhs, schema, payload, problems, structDefs);
            if (!lhs || !rhs) {
                return std::nullopt;
            }
            if (*lhs != ValueType::Bool || *rhs != ValueType::Bool) {
                problems.push_back({QStringLiteral("'%1' expects Bool on both sides, got %2 and %3")
                                        .arg(operatorText(node.kind), typeName(*lhs), typeName(*rhs)),
                                    node.sourceOffset});
                return std::nullopt;
            }
            return ValueType::Bool;
        }
        case NodeKind::Equal:
        case NodeKind::NotEqual: {
            const std::optional<ValueType> lhs = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
            const std::optional<ValueType> rhs = checkNode(ast, node.rhs, schema, payload, problems, structDefs);
            if (!lhs || !rhs) {
                return std::nullopt;
            }
            if (isNumeric(*lhs) && isNumeric(*rhs)) {
                return ValueType::Bool;
            }
            if (*lhs == *rhs) {
                return ValueType::Bool;
            }
            problems.push_back({QStringLiteral("cannot compare %1 and %2")
                                    .arg(typeName(*lhs), typeName(*rhs)),
                                node.sourceOffset});
            return std::nullopt;
        }
        case NodeKind::Less:
        case NodeKind::LessEqual:
        case NodeKind::Greater:
        case NodeKind::GreaterEqual: {
            const std::optional<ValueType> lhs = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
            const std::optional<ValueType> rhs = checkNode(ast, node.rhs, schema, payload, problems, structDefs);
            if (!lhs || !rhs) {
                return std::nullopt;
            }
            if (isNumeric(*lhs) && isNumeric(*rhs)) {
                return ValueType::Bool;
            }
            if (*lhs == ValueType::String && *rhs == ValueType::String) {
                return ValueType::Bool;
            }
            problems.push_back({QStringLiteral("'%1' requires numeric operands or both String, got %2 and %3")
                                    .arg(operatorText(node.kind), typeName(*lhs), typeName(*rhs)),
                                node.sourceOffset});
            return std::nullopt;
        }
        case NodeKind::Add: {
            const std::optional<ValueType> lhs = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
            const std::optional<ValueType> rhs = checkNode(ast, node.rhs, schema, payload, problems, structDefs);
            if (!lhs || !rhs) return std::nullopt;
            if (*lhs == ValueType::String && *rhs == ValueType::String) return ValueType::String;
            if (isNumeric(*lhs) && isNumeric(*rhs)) {
                return (*lhs == ValueType::Double || *rhs == ValueType::Double) ? ValueType::Double : ValueType::Int;
            }
            problems.push_back({QStringLiteral("'+' expects numeric operands or both String, got %1 and %2")
                                    .arg(typeName(*lhs), typeName(*rhs)),
                                node.sourceOffset});
            return std::nullopt;
        }
        case NodeKind::Subtract:
        case NodeKind::Multiply:
        case NodeKind::Divide: {
            const std::optional<ValueType> lhs = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
            const std::optional<ValueType> rhs = checkNode(ast, node.rhs, schema, payload, problems, structDefs);
            if (!lhs || !rhs) return std::nullopt;
            if (isNumeric(*lhs) && isNumeric(*rhs)) {
                if (node.kind == NodeKind::Divide) return ValueType::Double;
                return (*lhs == ValueType::Double || *rhs == ValueType::Double) ? ValueType::Double : ValueType::Int;
            }
            problems.push_back({QStringLiteral("'%1' expects numeric operands, got %2 and %3")
                                    .arg(operatorText(node.kind), typeName(*lhs), typeName(*rhs)),
                                node.sourceOffset});
            return std::nullopt;
        }
        case NodeKind::Modulo: {
            const std::optional<ValueType> lhs = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
            const std::optional<ValueType> rhs = checkNode(ast, node.rhs, schema, payload, problems, structDefs);
            if (!lhs || !rhs) return std::nullopt;
            if (*lhs == ValueType::Int && *rhs == ValueType::Int) {
                return ValueType::Int;
            }
            problems.push_back({QStringLiteral("'%' expects Int on both sides, got %1 and %2")
                                    .arg(typeName(*lhs), typeName(*rhs)),
                                node.sourceOffset});
            return std::nullopt;
        }
        case NodeKind::MemberAccess: {
            if (ast.nodes.at(static_cast<size_t>(node.lhs)).kind == NodeKind::Identifier &&
                ast.nodes.at(static_cast<size_t>(node.lhs)).text == QLatin1String("Math")) {
                if (node.text == QLatin1String("PI") || node.text == QLatin1String("E")) {
                    return ValueType::Double;
                }
                problems.push_back({QStringLiteral("unknown Math property '%1'").arg(node.text), node.sourceOffset});
                return std::nullopt;
            }
            const MemberChain chain = extractMemberChain(ast, index);
            if (!chain.valid) {
                problems.push_back({QStringLiteral("member access '%1' is not supported on this type").arg(node.text),
                                    node.sourceOffset});
                return std::nullopt;
            }
            if (payload.kind == PayloadKind::Event || (!payload.name.isEmpty() && chain.rootIdentifier == payload.name)) {
                QStringList props = chain.propertyChain;
                if (!props.isEmpty() && props.first() == QLatin1String("payload")) {
                    bool isFieldOnStruct = false;
                    if (!payload.structTypeName.isEmpty()) {
                        for (const StructDefinition& sd : structDefs) {
                            if (sd.name == payload.structTypeName) {
                                for (const StructField& f : sd.fields) {
                                    if (f.name == QLatin1String("payload")) {
                                        isFieldOnStruct = true;
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }
                    if (!isFieldOnStruct) {
                        props.removeFirst();
                    }
                }
                if (props.isEmpty()) {
                    return payload.type;
                }
                if (payload.type != ValueType::Object) {
                    problems.push_back({QStringLiteral("member access '%1' is not supported on scalar event payload of type %2")
                                            .arg(props.first(), typeName(payload.type)),
                                        node.sourceOffset});
                    return std::nullopt;
                }
                return checkStructMemberPath(payload.structTypeName, props, structDefs, node.sourceOffset, problems);
            }
            const ContextVariable* rootVar = nullptr;
            for (const ContextVariable& variable : schema) {
                if (variable.name == chain.rootIdentifier) {
                    rootVar = &variable;
                    break;
                }
            }
            if (!rootVar) {
                problems.push_back({QStringLiteral("unknown identifier '%1'").arg(chain.rootIdentifier), node.sourceOffset});
                return std::nullopt;
            }
            if (rootVar->type != ContextType::Object) {
                problems.push_back({QStringLiteral("member access '%1' is not supported on type %2")
                                        .arg(chain.propertyChain.first(), typeName(fromContextType(rootVar->type))),
                                    node.sourceOffset});
                return std::nullopt;
            }
            if (!rootVar->customTypeName.isEmpty()) {
                return checkStructMemberPath(rootVar->customTypeName, chain.propertyChain, structDefs, node.sourceOffset, problems);
            }
            QJsonParseError jsonErr;
            const QJsonDocument jsonDoc = QJsonDocument::fromJson(rootVar->initialValue.trimmed().toUtf8(), &jsonErr);
            if (jsonDoc.isNull() || !jsonDoc.isObject()) {
                problems.push_back({QStringLiteral("context object '%1' has invalid JSON initialValue").arg(rootVar->name),
                                    node.sourceOffset});
                return std::nullopt;
            }
            QJsonObject currObj = jsonDoc.object();
            for (int i = 0; i < chain.propertyChain.size(); ++i) {
                const QString& prop = chain.propertyChain.at(i);
                if (!currObj.contains(prop)) {
                    const QString parentName = (i == 0) ? chain.rootIdentifier : chain.propertyChain.at(i - 1);
                    problems.push_back({QStringLiteral("unknown property '%1' on object '%2'").arg(prop, parentName),
                                        node.sourceOffset});
                    return std::nullopt;
                }
                const QJsonValue val = currObj.value(prop);
                if (i == chain.propertyChain.size() - 1) {
                    if (val.isBool()) return ValueType::Bool;
                    if (val.isDouble()) {
                        const double num = val.toDouble();
                        const bool isInt = std::isfinite(num) && num == std::trunc(num) && std::fabs(num) < 9.2e18;
                        return isInt ? ValueType::Int : ValueType::Double;
                    }
                    if (val.isString()) return ValueType::String;
                    if (val.isObject()) return ValueType::Object;
                    problems.push_back({QStringLiteral("property '%1' has unsupported JSON type").arg(prop), node.sourceOffset});
                    return std::nullopt;
                } else {
                    if (!val.isObject()) {
                        problems.push_back({QStringLiteral("property '%1' is not an object").arg(prop), node.sourceOffset});
                        return std::nullopt;
                    }
                    currObj = val.toObject();
                }
            }
            return std::nullopt;
        }
        case NodeKind::Call: {
            const QString& fn = node.text;
            if (fn == QLatin1String("sqrt") || fn == QLatin1String("abs") || fn == QLatin1String("round") ||
                fn == QLatin1String("floor") || fn == QLatin1String("ceil")) {
                if (node.lhs == -1 || node.rhs != -1 || !node.extraArgs.empty()) {
                    problems.push_back({QStringLiteral("function '%1' expects 1 argument").arg(fn), node.sourceOffset});
                    return std::nullopt;
                }
                const std::optional<ValueType> arg = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
                if (!arg) return std::nullopt;
                if (!isNumeric(*arg)) {
                    problems.push_back({QStringLiteral("function '%1' expects numeric argument, got %2").arg(fn, typeName(*arg)),
                                        node.sourceOffset});
                    return std::nullopt;
                }
                return (fn == QLatin1String("abs")) ? *arg : ValueType::Double;
            }
            if (fn == QLatin1String("pow") || fn == QLatin1String("min") || fn == QLatin1String("max")) {
                if (node.lhs == -1 || node.rhs == -1 || !node.extraArgs.empty()) {
                    problems.push_back({QStringLiteral("function '%1' expects 2 arguments").arg(fn), node.sourceOffset});
                    return std::nullopt;
                }
                const std::optional<ValueType> a1 = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
                const std::optional<ValueType> a2 = checkNode(ast, node.rhs, schema, payload, problems, structDefs);
                if (!a1 || !a2) return std::nullopt;
                if (!isNumeric(*a1) || !isNumeric(*a2)) {
                    problems.push_back({QStringLiteral("function '%1' expects numeric arguments, got %2 and %3")
                                            .arg(fn, typeName(*a1), typeName(*a2)),
                                        node.sourceOffset});
                    return std::nullopt;
                }
                if (fn == QLatin1String("pow")) return ValueType::Double;
                return (*a1 == ValueType::Double || *a2 == ValueType::Double) ? ValueType::Double : ValueType::Int;
            }
            problems.push_back({QStringLiteral("unknown function '%1'").arg(fn), node.sourceOffset});
            return std::nullopt;
        }
        default: break;
    }

    // Every remaining kind is a comparison.
    const std::optional<ValueType> lhs = checkNode(ast, node.lhs, schema, payload, problems, structDefs);
    const std::optional<ValueType> rhs = checkNode(ast, node.rhs, schema, payload, problems, structDefs);
    if (!lhs || !rhs) {
        return std::nullopt;
    }
    // Int and Double mix; any other cross-type pair is an error, and Bool
    // supports equality only.
    const bool numericPair = isNumeric(*lhs) && isNumeric(*rhs);
    const bool stringPair = (*lhs == ValueType::String && *rhs == ValueType::String);
    const bool boolPair = (*lhs == ValueType::Bool && *rhs == ValueType::Bool);
    const bool acceptable = numericPair || stringPair || (isEquality(node.kind) && boolPair);
    if (!acceptable) {
        problems.push_back({QStringLiteral("'%1' cannot compare %2 with %3")
                                .arg(operatorText(node.kind), typeName(*lhs), typeName(*rhs)),
                            node.sourceOffset});
        return std::nullopt;
    }
    return ValueType::Bool;
}

}  // namespace

QVector<TypeProblem> typeCheck(const Ast& ast, const QVector<ContextVariable>& schema, const PayloadBinding& payload,
                                const QVector<StructDefinition>& structDefs) {
    QVector<TypeProblem> problems;
    if (ast.root < 0 || ast.root >= static_cast<int>(ast.nodes.size())) {
        problems.push_back({QStringLiteral("empty expression"), 0});
        return problems;
    }
    const std::optional<ValueType> top = checkNode(ast, ast.root, schema, payload, problems, structDefs);
    // A guard must be Bool at top level.
    if (top && *top != ValueType::Bool) {
        problems.push_back({QStringLiteral("a guard must be Bool, got %1").arg(typeName(*top)),
                            ast.nodes.at(static_cast<size_t>(ast.root)).sourceOffset});
    }
    return problems;
}

QString validateGuardSource(const QString& source, const QVector<ContextVariable>& schema, int* position,
                             const PayloadBinding& payload, const QVector<StructDefinition>& structDefs) {
    if (position != nullptr) {
        *position = 0;
    }
    // Blank ("no guard") and a bare identifier (a hook, even `output`/`error`)
    // are not expressions, so they are valid without parsing.
    if (source.trimmed().isEmpty() || isBareIdentifier(source)) {
        return QString();
    }
    const ParseResult parsed = parse(source);
    if (!parsed.ok) {
        if (position != nullptr) {
            *position = parsed.position;
        }
        return parsed.message;
    }
    const QVector<TypeProblem> problems = typeCheck(parsed.ast, schema, payload, structDefs);
    if (!problems.isEmpty()) {
        if (position != nullptr) {
            *position = problems.first().position;
        }
        return problems.first().message;
    }
    return QString();
}

}  // namespace app::expr
