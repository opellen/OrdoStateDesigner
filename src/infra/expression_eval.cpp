#include "infra/expression.h"

#include <QMetaType>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <optional>

namespace app::expr {

namespace {

// ---- evaluate ------------------------------------------------------------

std::optional<Value> valueFromVariant(const QVariant& variant) {
    switch (variant.typeId()) {
        case QMetaType::Bool: return Value::fromBool(variant.toBool());
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            return Value::fromInt(variant.toLongLong());
        case QMetaType::Float:
        case QMetaType::Double:
            return Value::fromDouble(variant.toDouble());
        case QMetaType::QString:
            return Value::fromString(variant.toString());
        case QMetaType::QVariantMap:
            return Value::fromObject(variant.toMap());
        default:
            if (variant.canConvert<QVariantMap>()) {
                return Value::fromObject(variant.toMap());
            }
            return std::nullopt;
    }
}

std::optional<Value> evalNode(const Ast& ast, int index, const QVariantMap& values);

// All six comparisons, dispatched on the operand pair: numeric, String or Bool.
std::optional<Value> evalComparison(NodeKind kind, const Value& lhs, const Value& rhs) {
    if (isNumeric(lhs.type) && isNumeric(rhs.type)) {
        // Int/Int stays integral so large qint64s compare exactly; only a
        // mixed pair promotes to Double.
        if (lhs.type == ValueType::Int && rhs.type == ValueType::Int) {
            const qint64 a = lhs.intValue;
            const qint64 b = rhs.intValue;
            switch (kind) {
                case NodeKind::Equal: return Value::fromBool(a == b);
                case NodeKind::NotEqual: return Value::fromBool(a != b);
                case NodeKind::Less: return Value::fromBool(a < b);
                case NodeKind::LessEqual: return Value::fromBool(a <= b);
                case NodeKind::Greater: return Value::fromBool(a > b);
                case NodeKind::GreaterEqual: return Value::fromBool(a >= b);
                default: return std::nullopt;
            }
        }
        const double a = lhs.asDouble();
        const double b = rhs.asDouble();
        switch (kind) {
            case NodeKind::Equal: return Value::fromBool(a == b);
            case NodeKind::NotEqual: return Value::fromBool(a != b);
            case NodeKind::Less: return Value::fromBool(a < b);
            case NodeKind::LessEqual: return Value::fromBool(a <= b);
            case NodeKind::Greater: return Value::fromBool(a > b);
            case NodeKind::GreaterEqual: return Value::fromBool(a >= b);
            default: return std::nullopt;
        }
    }
    if (lhs.type == ValueType::String && rhs.type == ValueType::String) {
        const int order = QString::compare(lhs.stringValue, rhs.stringValue);
        switch (kind) {
            case NodeKind::Equal: return Value::fromBool(order == 0);
            case NodeKind::NotEqual: return Value::fromBool(order != 0);
            case NodeKind::Less: return Value::fromBool(order < 0);
            case NodeKind::LessEqual: return Value::fromBool(order <= 0);
            case NodeKind::Greater: return Value::fromBool(order > 0);
            case NodeKind::GreaterEqual: return Value::fromBool(order >= 0);
            default: return std::nullopt;
        }
    }
    if (lhs.type == ValueType::Bool && rhs.type == ValueType::Bool) {
        if (kind == NodeKind::Equal) {
            return Value::fromBool(lhs.boolValue == rhs.boolValue);
        }
        if (kind == NodeKind::NotEqual) {
            return Value::fromBool(lhs.boolValue != rhs.boolValue);
        }
    }
    if (lhs.type == ValueType::Object && rhs.type == ValueType::Object) {
        if (kind == NodeKind::Equal) {
            return Value::fromBool(lhs.objectValue == rhs.objectValue);
        }
        if (kind == NodeKind::NotEqual) {
            return Value::fromBool(lhs.objectValue != rhs.objectValue);
        }
    }
    return std::nullopt;
}

bool isEventIdentifier(const Ast& ast, int nodeIndex) {
    if (nodeIndex >= 0 && nodeIndex < static_cast<int>(ast.nodes.size())) {
        const Node& n = ast.nodes.at(static_cast<size_t>(nodeIndex));
        return n.kind == NodeKind::Identifier && n.text == QLatin1String("event");
    }
    return false;
}

std::optional<Value> evalNode(const Ast& ast, int index, const QVariantMap& values) {
    const Node& node = ast.nodes.at(static_cast<size_t>(index));
    switch (node.kind) {
        case NodeKind::BoolLiteral: return Value::fromBool(node.boolValue);
        case NodeKind::IntLiteral: return Value::fromInt(node.intValue);
        case NodeKind::DoubleLiteral: return Value::fromDouble(node.doubleValue);
        case NodeKind::StringLiteral: return Value::fromString(node.text);
        case NodeKind::Identifier: {
            const auto it = values.constFind(node.text);
            if (it == values.constEnd()) {
                return std::nullopt;
            }
            return valueFromVariant(*it);
        }
        case NodeKind::Not: {
            const std::optional<Value> operand = evalNode(ast, node.lhs, values);
            if (!operand || operand->type != ValueType::Bool) {
                return std::nullopt;
            }
            return Value::fromBool(!operand->boolValue);
        }
        case NodeKind::Negate: {
            const std::optional<Value> operand = evalNode(ast, node.lhs, values);
            if (!operand) {
                return std::nullopt;
            }
            if (operand->type == ValueType::Int) {
                return Value::fromInt(-operand->intValue);
            }
            if (operand->type == ValueType::Double) {
                return Value::fromDouble(-operand->doubleValue);
            }
            return std::nullopt;
        }
        case NodeKind::And:
        case NodeKind::Or: {
            // No short-circuit: evaluation is side-effect free.
            const std::optional<Value> lhs = evalNode(ast, node.lhs, values);
            const std::optional<Value> rhs = evalNode(ast, node.rhs, values);
            if (!lhs || !rhs || lhs->type != ValueType::Bool || rhs->type != ValueType::Bool) {
                return std::nullopt;
            }
            return Value::fromBool(node.kind == NodeKind::And ? (lhs->boolValue && rhs->boolValue)
                                                              : (lhs->boolValue || rhs->boolValue));
        }
        case NodeKind::Add: {
            const std::optional<Value> lhs = evalNode(ast, node.lhs, values);
            const std::optional<Value> rhs = evalNode(ast, node.rhs, values);
            if (!lhs || !rhs) return std::nullopt;
            if (lhs->type == ValueType::String && rhs->type == ValueType::String) {
                return Value::fromString(lhs->stringValue + rhs->stringValue);
            }
            if (lhs->type == ValueType::Int && rhs->type == ValueType::Int) {
                return Value::fromInt(lhs->intValue + rhs->intValue);
            }
            if (isNumeric(lhs->type) && isNumeric(rhs->type)) {
                return Value::fromDouble(lhs->asDouble() + rhs->asDouble());
            }
            return std::nullopt;
        }
        case NodeKind::Subtract: {
            const std::optional<Value> lhs = evalNode(ast, node.lhs, values);
            const std::optional<Value> rhs = evalNode(ast, node.rhs, values);
            if (!lhs || !rhs) return std::nullopt;
            if (lhs->type == ValueType::Int && rhs->type == ValueType::Int) {
                return Value::fromInt(lhs->intValue - rhs->intValue);
            }
            if (isNumeric(lhs->type) && isNumeric(rhs->type)) {
                return Value::fromDouble(lhs->asDouble() - rhs->asDouble());
            }
            return std::nullopt;
        }
        case NodeKind::Multiply: {
            const std::optional<Value> lhs = evalNode(ast, node.lhs, values);
            const std::optional<Value> rhs = evalNode(ast, node.rhs, values);
            if (!lhs || !rhs) return std::nullopt;
            if (lhs->type == ValueType::Int && rhs->type == ValueType::Int) {
                return Value::fromInt(lhs->intValue * rhs->intValue);
            }
            if (isNumeric(lhs->type) && isNumeric(rhs->type)) {
                return Value::fromDouble(lhs->asDouble() * rhs->asDouble());
            }
            return std::nullopt;
        }
        case NodeKind::Divide: {
            const std::optional<Value> lhs = evalNode(ast, node.lhs, values);
            const std::optional<Value> rhs = evalNode(ast, node.rhs, values);
            if (!lhs || !rhs) return std::nullopt;
            if (rhs->asDouble() == 0.0) return std::nullopt; // division by zero safety
            if (lhs->type == ValueType::Int && rhs->type == ValueType::Int) {
                if (rhs->intValue == 0) return std::nullopt;
                return Value::fromInt(lhs->intValue / rhs->intValue);
            }
            if (isNumeric(lhs->type) && isNumeric(rhs->type)) {
                return Value::fromDouble(lhs->asDouble() / rhs->asDouble());
            }
            return std::nullopt;
        }
        case NodeKind::Modulo: {
            const std::optional<Value> lhs = evalNode(ast, node.lhs, values);
            const std::optional<Value> rhs = evalNode(ast, node.rhs, values);
            if (!lhs || !rhs || lhs->type != ValueType::Int || rhs->type != ValueType::Int) return std::nullopt;
            if (rhs->intValue == 0) return std::nullopt; // modulo by zero safety
            return Value::fromInt(lhs->intValue % rhs->intValue);
        }
        case NodeKind::MemberAccess: {
            if (ast.nodes.at(static_cast<size_t>(node.lhs)).kind == NodeKind::Identifier &&
                ast.nodes.at(static_cast<size_t>(node.lhs)).text == QLatin1String("Math")) {
                if (node.text == QLatin1String("PI")) return Value::fromDouble(3.14159265358979323846);
                if (node.text == QLatin1String("E")) return Value::fromDouble(2.71828182845904523536);
            }
            const auto lhsVal = evalNode(ast, node.lhs, values);
            if (!lhsVal) {
                return std::nullopt;
            }
            if (node.text == QLatin1String("payload") && isEventIdentifier(ast, node.lhs)) {
                if (lhsVal->type != ValueType::Object || !lhsVal->objectValue.contains(QLatin1String("payload"))) {
                    return lhsVal;
                }
            }
            if (lhsVal->type != ValueType::Object) {
                return std::nullopt;
            }
            const auto it = lhsVal->objectValue.constFind(node.text);
            if (it == lhsVal->objectValue.constEnd()) {
                return std::nullopt;
            }
            return valueFromVariant(*it);
        }
        case NodeKind::Call: {
            const QString& fn = node.text;
            if (fn == QLatin1String("sqrt")) {
                const auto arg = evalNode(ast, node.lhs, values);
                if (!arg || !isNumeric(arg->type) || arg->asDouble() < 0.0) return std::nullopt;
                return Value::fromDouble(std::sqrt(arg->asDouble()));
            }
            if (fn == QLatin1String("abs")) {
                const auto arg = evalNode(ast, node.lhs, values);
                if (!arg || !isNumeric(arg->type)) return std::nullopt;
                if (arg->type == ValueType::Int) return Value::fromInt(std::abs(arg->intValue));
                return Value::fromDouble(std::abs(arg->asDouble()));
            }
            if (fn == QLatin1String("round")) {
                const auto arg = evalNode(ast, node.lhs, values);
                if (!arg || !isNumeric(arg->type)) return std::nullopt;
                return Value::fromDouble(std::round(arg->asDouble()));
            }
            if (fn == QLatin1String("floor")) {
                const auto arg = evalNode(ast, node.lhs, values);
                if (!arg || !isNumeric(arg->type)) return std::nullopt;
                return Value::fromDouble(std::floor(arg->asDouble()));
            }
            if (fn == QLatin1String("ceil")) {
                const auto arg = evalNode(ast, node.lhs, values);
                if (!arg || !isNumeric(arg->type)) return std::nullopt;
                return Value::fromDouble(std::ceil(arg->asDouble()));
            }
            if (fn == QLatin1String("pow")) {
                const auto a1 = evalNode(ast, node.lhs, values);
                const auto a2 = evalNode(ast, node.rhs, values);
                if (!a1 || !a2 || !isNumeric(a1->type) || !isNumeric(a2->type)) return std::nullopt;
                return Value::fromDouble(std::pow(a1->asDouble(), a2->asDouble()));
            }
            if (fn == QLatin1String("min")) {
                const auto a1 = evalNode(ast, node.lhs, values);
                const auto a2 = evalNode(ast, node.rhs, values);
                if (!a1 || !a2 || !isNumeric(a1->type) || !isNumeric(a2->type)) return std::nullopt;
                if (a1->type == ValueType::Int && a2->type == ValueType::Int) {
                    return Value::fromInt(std::min(a1->intValue, a2->intValue));
                }
                return Value::fromDouble(std::min(a1->asDouble(), a2->asDouble()));
            }
            if (fn == QLatin1String("max")) {
                const auto a1 = evalNode(ast, node.lhs, values);
                const auto a2 = evalNode(ast, node.rhs, values);
                if (!a1 || !a2 || !isNumeric(a1->type) || !isNumeric(a2->type)) return std::nullopt;
                if (a1->type == ValueType::Int && a2->type == ValueType::Int) {
                    return Value::fromInt(std::max(a1->intValue, a2->intValue));
                }
                return Value::fromDouble(std::max(a1->asDouble(), a2->asDouble()));
            }
            return std::nullopt;
        }
        default: break;
    }

    const std::optional<Value> lhs = evalNode(ast, node.lhs, values);
    const std::optional<Value> rhs = evalNode(ast, node.rhs, values);
    if (!lhs || !rhs) {
        return std::nullopt;
    }
    return evalComparison(node.kind, *lhs, *rhs);
}

}  // namespace

Value evaluate(const Ast& ast, const QVariantMap& values, bool* ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (ast.root < 0 || ast.root >= static_cast<int>(ast.nodes.size())) {
        return Value();
    }
    const std::optional<Value> result = evalNode(ast, ast.root, values);
    if (!result) {
        return Value();
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return *result;
}

}  // namespace app::expr
