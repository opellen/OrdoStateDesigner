#include "infra/expression.h"

#include <QChar>
#include <QSet>
#include <QString>

namespace app::expr {

namespace {

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

// Same ladder as the parser, expressed as numbers: a child needs parentheses
// exactly when its own binding is looser than the slot it sits in.
int precedenceOf(NodeKind kind) {
    switch (kind) {
        case NodeKind::Or: return 1;
        case NodeKind::And: return 2;
        case NodeKind::Equal:
        case NodeKind::NotEqual:
        case NodeKind::Less:
        case NodeKind::LessEqual:
        case NodeKind::Greater:
        case NodeKind::GreaterEqual:
            return 3;
        case NodeKind::Add:
        case NodeKind::Subtract:
            return 4;
        case NodeKind::Multiply:
        case NodeKind::Divide:
        case NodeKind::Modulo:
            return 5;
        case NodeKind::Not:
        case NodeKind::Negate:
            return 6;
        default:
            return 7;
    }
}

// Shortest text that round-trips exactly, always with a '.' or exponent so it
// re-parses as a Double (`3.0`, never `3`).
QString printDouble(double value) {
    QString text = QString::number(value, 'g', 17);
    for (int precision = 1; precision < 17; ++precision) {
        const QString candidate = QString::number(value, 'g', precision);
        if (candidate.toDouble() == value) {
            text = candidate;
            break;
        }
    }
    if (!text.contains(QLatin1Char('.')) && !text.contains(QLatin1Char('e')) &&
        !text.contains(QLatin1Char('E')) && !text.contains(QLatin1Char('n')) &&
        !text.contains(QLatin1Char('i'))) {
        text.append(QLatin1String(".0"));
    }
    return text;
}

QString printNode(const Ast& ast, int index, int minPrecedence) {
    const Node& node = ast.nodes.at(static_cast<size_t>(index));
    QString text;
    switch (node.kind) {
        case NodeKind::BoolLiteral:
            text = node.boolValue ? QStringLiteral("true") : QStringLiteral("false");
            break;
        case NodeKind::IntLiteral:
            text = QString::number(node.intValue);
            break;
        case NodeKind::DoubleLiteral:
            text = printDouble(node.doubleValue);
            break;
        case NodeKind::StringLiteral:
            // No escaping: the tokenizer never yields a value with a single quote.
            text = QLatin1Char('\'') + node.text + QLatin1Char('\'');
            break;
        case NodeKind::Identifier:
            text = node.text;
            break;
        case NodeKind::Not:
        case NodeKind::Negate:
            text = operatorText(node.kind) + printNode(ast, node.lhs, 6);
            break;
        case NodeKind::MemberAccess:
            text = printNode(ast, node.lhs, 7) + QLatin1Char('.') + node.text;
            break;
        case NodeKind::Call: {
            text = node.text + QLatin1Char('(');
            if (node.lhs != -1) {
                text += printNode(ast, node.lhs, 1);
            }
            if (node.rhs != -1) {
                text += QStringLiteral(", ") + printNode(ast, node.rhs, 1);
            }
            for (int extra : node.extraArgs) {
                text += QStringLiteral(", ") + printNode(ast, extra, 1);
            }
            text += QLatin1Char(')');
            break;
        }
        default: {
            // Left-associative: the right child sits one level tighter.
            const int own = precedenceOf(node.kind);
            text = printNode(ast, node.lhs, own) + QStringLiteral(" ") + operatorText(node.kind) +
                   QStringLiteral(" ") + printNode(ast, node.rhs, own + 1);
            break;
        }
    }
    if (precedenceOf(node.kind) < minPrecedence) {
        return QStringLiteral("(") + text + QStringLiteral(")");
    }
    return text;
}

// ---- emitCpp ---------------------------------------------------------------

// Leaves are the only nodes emitted without wrapping parentheses.
bool isLeafNode(NodeKind kind) {
    switch (kind) {
        case NodeKind::BoolLiteral:
        case NodeKind::IntLiteral:
        case NodeKind::DoubleLiteral:
        case NodeKind::StringLiteral:
        case NodeKind::Identifier:
        case NodeKind::Call:
        case NodeKind::MemberAccess:
            return true;
        default:
            return false;
    }
}

bool isEventIdentifier(const Ast& ast, int nodeIndex) {
    if (nodeIndex >= 0 && nodeIndex < static_cast<int>(ast.nodes.size())) {
        const Node& n = ast.nodes.at(static_cast<size_t>(nodeIndex));
        return n.kind == NodeKind::Identifier && n.text == QLatin1String("event");
    }
    return false;
}

QString emitCppNode(const Ast& ast, int index, const QString& contextExpr, const QSet<QString>& bareIdentifiers);

// Every non-leaf operand is parenthesized; do not reuse print()'s precedence
// elision. Our grammar puts all comparisons on one level but C++ does not
// (`a == b < c` would regroup), so only explicit parens match the evaluator.
QString emitCppOperand(const Ast& ast, int index, const QString& contextExpr, const QSet<QString>& bareIdentifiers) {
    const QString text = emitCppNode(ast, index, contextExpr, bareIdentifiers);
    if (isLeafNode(ast.nodes.at(static_cast<size_t>(index)).kind)) {
        return text;
    }
    return QLatin1Char('(') + text + QLatin1Char(')');
}

// Source strings have no escapes and may contain '"' or '\', which must be
// escaped for a C++ literal or the generated code breaks.
QString escapeCppStringLiteral(const QString& value) {
    QString out;
    out.reserve(value.size() + 2);
    for (const QChar ch : value) {
        switch (ch.unicode()) {
            case '\\': out += QLatin1String("\\\\"); break;
            case '"': out += QLatin1String("\\\""); break;
            case '\n': out += QLatin1String("\\n"); break;
            case '\r': out += QLatin1String("\\r"); break;
            case '\t': out += QLatin1String("\\t"); break;
            default: out += ch; break;
        }
    }
    return QLatin1Char('"') + out + QLatin1Char('"');
}

QString emitCppNode(const Ast& ast, int index, const QString& contextExpr, const QSet<QString>& bareIdentifiers) {
    const Node& node = ast.nodes.at(static_cast<size_t>(index));
    switch (node.kind) {
        case NodeKind::BoolLiteral:
            return node.boolValue ? QStringLiteral("true") : QStringLiteral("false");
        case NodeKind::IntLiteral:
            // LL matches the generated `long long` Int members.
            return QString::number(node.intValue) + QStringLiteral("LL");
        case NodeKind::DoubleLiteral:
            // printDouble() always yields a '.' or exponent, so C++ reads a double.
            return printDouble(node.doubleValue);
        case NodeKind::StringLiteral:
            return escapeCppStringLiteral(node.text);
        case NodeKind::Identifier:
            // Payload names are the emitted function's own parameters, not
            // Context members.
            if (bareIdentifiers.contains(node.text)) {
                return node.text;
            }
            return contextExpr + QLatin1Char('.') + node.text;
        case NodeKind::Not:
        case NodeKind::Negate:
            return operatorText(node.kind) + emitCppOperand(ast, node.lhs, contextExpr, bareIdentifiers);
        case NodeKind::MemberAccess: {
            if (ast.nodes.at(static_cast<size_t>(node.lhs)).kind == NodeKind::Identifier &&
                ast.nodes.at(static_cast<size_t>(node.lhs)).text == QLatin1String("Math")) {
                if (node.text == QLatin1String("PI")) return QStringLiteral("3.14159265358979323846");
                if (node.text == QLatin1String("E")) return QStringLiteral("2.71828182845904523536");
            }
            if (node.text == QLatin1String("payload") && isEventIdentifier(ast, node.lhs)) {
                return emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers);
            }
            return emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) + QLatin1Char('.') + node.text;
        }
        case NodeKind::Call: {
            const QString& fn = node.text;
            if (fn == QLatin1String("pow")) {
                return QStringLiteral("std::pow(") + emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) +
                       QStringLiteral(", ") + emitCppNode(ast, node.rhs, contextExpr, bareIdentifiers) + QStringLiteral(")");
            }
            if (fn == QLatin1String("sqrt")) {
                return QStringLiteral("std::sqrt(") + emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) + QStringLiteral(")");
            }
            if (fn == QLatin1String("abs")) {
                return QStringLiteral("std::abs(") + emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) + QStringLiteral(")");
            }
            if (fn == QLatin1String("round")) {
                return QStringLiteral("std::round(") + emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) + QStringLiteral(")");
            }
            if (fn == QLatin1String("floor")) {
                return QStringLiteral("std::floor(") + emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) + QStringLiteral(")");
            }
            if (fn == QLatin1String("ceil")) {
                return QStringLiteral("std::ceil(") + emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) + QStringLiteral(")");
            }
            if (fn == QLatin1String("min")) {
                return QStringLiteral("std::min(") + emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) +
                       QStringLiteral(", ") + emitCppNode(ast, node.rhs, contextExpr, bareIdentifiers) + QStringLiteral(")");
            }
            if (fn == QLatin1String("max")) {
                return QStringLiteral("std::max(") + emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers) +
                       QStringLiteral(", ") + emitCppNode(ast, node.rhs, contextExpr, bareIdentifiers) + QStringLiteral(")");
            }
            QString text = fn + QLatin1Char('(');
            if (node.lhs != -1) text += emitCppNode(ast, node.lhs, contextExpr, bareIdentifiers);
            if (node.rhs != -1) text += QStringLiteral(", ") + emitCppNode(ast, node.rhs, contextExpr, bareIdentifiers);
            text += QLatin1Char(')');
            return text;
        }
        default:
            return emitCppOperand(ast, node.lhs, contextExpr, bareIdentifiers) + QStringLiteral(" ") +
                   operatorText(node.kind) + QStringLiteral(" ") +
                   emitCppOperand(ast, node.rhs, contextExpr, bareIdentifiers);
    }
}

}  // namespace

QString print(const Ast& ast) {
    if (ast.root < 0 || ast.root >= static_cast<int>(ast.nodes.size())) {
        return QString();
    }
    return printNode(ast, ast.root, 1);
}

QString emitCpp(const Ast& ast, const QString& contextExpr, const QSet<QString>& bareIdentifiers) {
    if (ast.root < 0 || ast.root >= static_cast<int>(ast.nodes.size())) {
        return QString();
    }
    // The root is nobody's operand, so it is never wrapped.
    return emitCppNode(ast, ast.root, contextExpr, bareIdentifiers);
}

}  // namespace app::expr
