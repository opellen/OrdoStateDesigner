// --smoke phase 2d: the guard-expression unit.
// Sections 1-7 construct nothing (pure table drive over seven entry points).
// Integration tests are in harness/smoke_expression_integration.cpp.

#include <QJsonObject>
#include <QJsonValue>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <cstdio>
#include <memory>

#include "harness/harness.h"
#include "infra/expression.h"
#include "model/machine.h"

namespace {
// A fully parenthesised, kind-tagged dump (not app::expr::print(), whose
// minimal form elides the parentheses this phase must see to prove that
// `a || b && c` groups as `a || (b && c)`). The literal tags (`int:` / `dbl:` /
// `str:` / `id:`) pin which NodeKind the tokenizer chose, so `3` vs `3.0`
// cannot pass by accident.
QString dumpNode(const app::expr::Ast& ast, int index) {
    using app::expr::NodeKind;
    const app::expr::Node& node = ast.nodes.at(static_cast<size_t>(index));
    switch (node.kind) {
        case NodeKind::BoolLiteral:
            return node.boolValue ? QStringLiteral("true") : QStringLiteral("false");
        case NodeKind::IntLiteral:
            return QStringLiteral("int:") + QString::number(node.intValue);
        case NodeKind::DoubleLiteral:
            return QStringLiteral("dbl:") + QString::number(node.doubleValue);
        case NodeKind::StringLiteral:
            return QStringLiteral("str:") + node.text;
        case NodeKind::Identifier:
            return QStringLiteral("id:") + node.text;
        case NodeKind::Not:
            return QStringLiteral("(! ") + dumpNode(ast, node.lhs) + QStringLiteral(")");
        case NodeKind::Negate:
            return QStringLiteral("(- ") + dumpNode(ast, node.lhs) + QStringLiteral(")");
        case NodeKind::MemberAccess:
            return dumpNode(ast, node.lhs) + QStringLiteral(".") + node.text;
        case NodeKind::Call: {
            QString res = QStringLiteral("call:") + node.text + QStringLiteral("(");
            if (node.lhs != -1) res += dumpNode(ast, node.lhs);
            if (node.rhs != -1) res += QStringLiteral(", ") + dumpNode(ast, node.rhs);
            for (int extra : node.extraArgs) res += QStringLiteral(", ") + dumpNode(ast, extra);
            res += QStringLiteral(")");
            return res;
        }
        default: break;
    }
    QString op;
    switch (node.kind) {
        case NodeKind::And: op = QStringLiteral("&&"); break;
        case NodeKind::Or: op = QStringLiteral("||"); break;
        case NodeKind::Equal: op = QStringLiteral("=="); break;
        case NodeKind::NotEqual: op = QStringLiteral("!="); break;
        case NodeKind::Less: op = QStringLiteral("<"); break;
        case NodeKind::LessEqual: op = QStringLiteral("<="); break;
        case NodeKind::Greater: op = QStringLiteral(">"); break;
        case NodeKind::GreaterEqual: op = QStringLiteral(">="); break;
        case NodeKind::Add: op = QStringLiteral("+"); break;
        case NodeKind::Subtract: op = QStringLiteral("-"); break;
        case NodeKind::Multiply: op = QStringLiteral("*"); break;
        case NodeKind::Divide: op = QStringLiteral("/"); break;
        case NodeKind::Modulo: op = QStringLiteral("%"); break;
        default: op = QStringLiteral("?"); break;
    }
    return QStringLiteral("(") + dumpNode(ast, node.lhs) + QStringLiteral(" ") + op + QStringLiteral(" ") +
           dumpNode(ast, node.rhs) + QStringLiteral(")");
}

QString dump(const app::expr::Ast& ast) {
    if (ast.root < 0) {
        return QStringLiteral("<none>");
    }
    return dumpNode(ast, ast.root);
}

// Each expect* helper prints its own FAIL line and returns false, so a whole
// table runs to completion and every broken row shows in one log. The caller
// gates on the section's failure count.
bool expectStructure(const QString& source, const QString& wanted) {
    const app::expr::ParseResult parsed = app::expr::parse(source);
    if (!parsed.ok) {
        std::fprintf(stderr, "FAIL: expression parse(\"%s\") failed: %s (at %d)\n", qUtf8Printable(source),
                     qUtf8Printable(parsed.message), parsed.position);
        return false;
    }
    const QString actual = dump(parsed.ast);
    if (actual != wanted) {
        std::fprintf(stderr, "FAIL: expression parse(\"%s\") grouped as %s (want %s)\n", qUtf8Printable(source),
                     qUtf8Printable(actual), qUtf8Printable(wanted));
        return false;
    }
    return true;
}

bool expectParseFailure(const QString& source, int wantedPosition) {
    const app::expr::ParseResult parsed = app::expr::parse(source);
    if (parsed.ok) {
        std::fprintf(stderr, "FAIL: expression parse(\"%s\") succeeded (want a failure at %d)\n",
                     qUtf8Printable(source), wantedPosition);
        return false;
    }
    if (parsed.ast.root != -1) {
        std::fprintf(stderr, "FAIL: expression parse(\"%s\") failed but left ast.root=%d (want -1)\n",
                     qUtf8Printable(source), parsed.ast.root);
        return false;
    }
    if (parsed.message.isEmpty()) {
        std::fprintf(stderr, "FAIL: expression parse(\"%s\") failed with an empty message\n", qUtf8Printable(source));
        return false;
    }
    if (parsed.position != wantedPosition) {
        std::fprintf(stderr, "FAIL: expression parse(\"%s\") reported position %d (want %d) -- \"%s\"\n",
                     qUtf8Printable(source), parsed.position, wantedPosition, qUtf8Printable(parsed.message));
        return false;
    }
    return true;
}

bool expectBareIdentifier(const QString& source, bool wanted) {
    const bool actual = app::expr::isBareIdentifier(source);
    if (actual != wanted) {
        std::fprintf(stderr, "FAIL: isBareIdentifier(\"%s\")=%s (want %s)\n", qUtf8Printable(source),
                     actual ? "true" : "false", wanted ? "true" : "false");
        return false;
    }
    return true;
}

// `wantedProblems` == 0 asserts a well-typed expression; anything else asserts
// that exact finding count, so a cascade of duplicate findings fails here.
bool expectTypeProblems(const QString& source, const QVector<app::ContextVariable>& schema, int wantedProblems) {
    const app::expr::ParseResult parsed = app::expr::parse(source);
    if (!parsed.ok) {
        std::fprintf(stderr, "FAIL: typeCheck fixture \"%s\" did not even parse: %s (at %d)\n",
                     qUtf8Printable(source), qUtf8Printable(parsed.message), parsed.position);
        return false;
    }
    const QVector<app::expr::TypeProblem> problems = app::expr::typeCheck(parsed.ast, schema);
    if (problems.size() != wantedProblems) {
        QStringList rendered;
        for (const app::expr::TypeProblem& problem : problems) {
            rendered << QStringLiteral("%1@%2").arg(problem.message).arg(problem.position);
        }
        std::fprintf(stderr, "FAIL: typeCheck(\"%s\") reported %d problem(s) (want %d): %s\n", qUtf8Printable(source),
                     static_cast<int>(problems.size()), wantedProblems, qUtf8Printable(rendered.join(QStringLiteral("; "))));
        return false;
    }
    // validateGuardSource() is the single-string facade over this chain and
    // must agree with it on every row. A bare identifier is excluded: the facade
    // returns early there by design (it is a hook, not an expression).
    if (!app::expr::isBareIdentifier(source)) {
        const QString flattened = app::expr::validateGuardSource(source, schema, nullptr);
        if (flattened.isEmpty() != (wantedProblems == 0)) {
            std::fprintf(stderr,
                         "FAIL: validateGuardSource(\"%s\") returned \"%s\" but typeCheck found %d problem(s)\n",
                         qUtf8Printable(source), qUtf8Printable(flattened), wantedProblems);
            return false;
        }
    }
    return true;
}

bool expectBoolResult(const QString& source, const QVariantMap& values, bool wanted) {
    const app::expr::ParseResult parsed = app::expr::parse(source);
    if (!parsed.ok) {
        std::fprintf(stderr, "FAIL: evaluate fixture \"%s\" did not parse: %s\n", qUtf8Printable(source),
                     qUtf8Printable(parsed.message));
        return false;
    }
    bool ok = false;
    const app::expr::Value value = app::expr::evaluate(parsed.ast, values, &ok);
    if (!ok) {
        std::fprintf(stderr, "FAIL: evaluate(\"%s\") set ok=false (want %s)\n", qUtf8Printable(source),
                     wanted ? "true" : "false");
        return false;
    }
    if (value.type != app::expr::ValueType::Bool || value.boolValue != wanted) {
        std::fprintf(stderr, "FAIL: evaluate(\"%s\") = %s (want Bool %s)\n", qUtf8Printable(source),
                     value.type == app::expr::ValueType::Bool ? (value.boolValue ? "Bool true" : "Bool false")
                                                              : "a non-Bool",
                     wanted ? "true" : "false");
        return false;
    }
    return true;
}

bool expectEvaluateNotOk(const QString& source, const QVariantMap& values) {
    const app::expr::ParseResult parsed = app::expr::parse(source);
    if (!parsed.ok) {
        std::fprintf(stderr, "FAIL: evaluate fixture \"%s\" did not parse: %s\n", qUtf8Printable(source),
                     qUtf8Printable(parsed.message));
        return false;
    }
    bool ok = true;
    app::expr::evaluate(parsed.ast, values, &ok);
    if (ok) {
        std::fprintf(stderr, "FAIL: evaluate(\"%s\") set ok=true against an incomplete value map\n",
                     qUtf8Printable(source));
        return false;
    }
    return true;
}

// Two assertions per row: the canonical form is as wanted, and re-parsing it
// prints the same thing again (fixed point). The second catches a printer that
// drops a needed parenthesis.
bool expectPrint(const QString& source, const QString& wanted) {
    const app::expr::ParseResult first = app::expr::parse(source);
    if (!first.ok) {
        std::fprintf(stderr, "FAIL: print fixture \"%s\" did not parse: %s\n", qUtf8Printable(source),
                     qUtf8Printable(first.message));
        return false;
    }
    const QString printed = app::expr::print(first.ast);
    if (printed != wanted) {
        std::fprintf(stderr, "FAIL: print(parse(\"%s\")) = \"%s\" (want \"%s\")\n", qUtf8Printable(source),
                     qUtf8Printable(printed), qUtf8Printable(wanted));
        return false;
    }
    const app::expr::ParseResult second = app::expr::parse(printed);
    if (!second.ok) {
        std::fprintf(stderr, "FAIL: print(parse(\"%s\")) = \"%s\" does not re-parse: %s\n", qUtf8Printable(source),
                     qUtf8Printable(printed), qUtf8Printable(second.message));
        return false;
    }
    if (dump(second.ast) != dump(first.ast)) {
        std::fprintf(stderr, "FAIL: print(parse(\"%s\")) regrouped: %s -> %s\n", qUtf8Printable(source),
                     qUtf8Printable(dump(first.ast)), qUtf8Printable(dump(second.ast)));
        return false;
    }
    if (app::expr::print(second.ast) != printed) {
        std::fprintf(stderr, "FAIL: print is not a fixed point for \"%s\": \"%s\" -> \"%s\"\n", qUtf8Printable(source),
                     qUtf8Printable(printed), qUtf8Printable(app::expr::print(second.ast)));
        return false;
    }
    return true;
}

bool expectRename(const QString& source, const QString& before, const QString& after, const QString& wanted) {
    const QString actual = app::expr::renameIdentifierInSource(source, before, after);
    if (actual != wanted) {
        std::fprintf(stderr, "FAIL: renameIdentifierInSource(\"%s\", %s -> %s) = \"%s\" (want \"%s\")\n",
                     qUtf8Printable(source), qUtf8Printable(before), qUtf8Printable(after), qUtf8Printable(actual),
                     qUtf8Printable(wanted));
        return false;
    }
    return true;
}


app::ContextVariable variable(const QString& name, app::ContextType type) {
    app::ContextVariable result;
    result.name = name;
    result.type = type;
    return result;
}


}  // namespace

int runExpressionSmoke() {
    using app::expr::NodeKind;
    using app::expr::ParseResult;

    int failures = 0;
    // ---- 1. tokenizer + parser: literals, every operator, precedence,
    // associativity, parenthesis override, unary chains ----------------------
    failures += !expectStructure(QStringLiteral("true"), QStringLiteral("true"));
    failures += !expectStructure(QStringLiteral("false"), QStringLiteral("false"));
    failures += !expectStructure(QStringLiteral("123"), QStringLiteral("int:123"));
    failures += !expectStructure(QStringLiteral("3.5"), QStringLiteral("dbl:3.5"));
    failures += !expectStructure(QStringLiteral("'admin'"), QStringLiteral("str:admin"));
    // An empty string literal and one containing spaces both survive: the
    // tokenizer stops at the closing quote only, never at whitespace.
    failures += !expectStructure(QStringLiteral("role == ''"), QStringLiteral("(id:role == str:)"));
    failures += !expectStructure(QStringLiteral("role == 'super admin'"), QStringLiteral("(id:role == str:super admin)"));
    // parse() is grammar-only: a bare identifier is a perfectly good
    // expression HERE. Only isBareIdentifier() (section 3) knows it is a hook.
    failures += !expectStructure(QStringLiteral("count"), QStringLiteral("id:count"));

    failures += !expectStructure(QStringLiteral("a == b"), QStringLiteral("(id:a == id:b)"));
    failures += !expectStructure(QStringLiteral("a != b"), QStringLiteral("(id:a != id:b)"));
    failures += !expectStructure(QStringLiteral("a < b"), QStringLiteral("(id:a < id:b)"));
    failures += !expectStructure(QStringLiteral("a <= b"), QStringLiteral("(id:a <= id:b)"));
    failures += !expectStructure(QStringLiteral("a > b"), QStringLiteral("(id:a > id:b)"));
    failures += !expectStructure(QStringLiteral("a >= b"), QStringLiteral("(id:a >= id:b)"));
    failures += !expectStructure(QStringLiteral("a && b"), QStringLiteral("(id:a && id:b)"));
    failures += !expectStructure(QStringLiteral("a || b"), QStringLiteral("(id:a || id:b)"));

    // Arithmetic operators: +, -, *, /, %
    failures += !expectStructure(QStringLiteral("a + b"), QStringLiteral("(id:a + id:b)"));
    failures += !expectStructure(QStringLiteral("a - b"), QStringLiteral("(id:a - id:b)"));
    failures += !expectStructure(QStringLiteral("a * b"), QStringLiteral("(id:a * id:b)"));
    failures += !expectStructure(QStringLiteral("a / b"), QStringLiteral("(id:a / id:b)"));
    failures += !expectStructure(QStringLiteral("a % b"), QStringLiteral("(id:a % id:b)"));

    // Precedence: multiplicative (* / %) > additive (+ -) > comparison > && > ||
    failures += !expectStructure(QStringLiteral("a + b * c"), QStringLiteral("(id:a + (id:b * id:c))"));
    failures += !expectStructure(QStringLiteral("a * b + c"), QStringLiteral("((id:a * id:b) + id:c)"));
    failures += !expectStructure(QStringLiteral("a - b / c"), QStringLiteral("(id:a - (id:b / id:c))"));
    failures += !expectStructure(QStringLiteral("a % b + c"), QStringLiteral("((id:a % id:b) + id:c)"));
    failures += !expectStructure(QStringLiteral("1 + 2 * 3 < 10"), QStringLiteral("((int:1 + (int:2 * int:3)) < int:10)"));
    failures += !expectStructure(QStringLiteral("(a + b) * c"), QStringLiteral("((id:a + id:b) * id:c)"));

    // Function calls & Math.<fn> normalization
    failures += !expectStructure(QStringLiteral("sqrt(16)"), QStringLiteral("call:sqrt(int:16)"));
    failures += !expectStructure(QStringLiteral("Math.sqrt(16)"), QStringLiteral("call:sqrt(int:16)"));
    failures += !expectStructure(QStringLiteral("pow(2, 3)"), QStringLiteral("call:pow(int:2, int:3)"));
    failures += !expectStructure(QStringLiteral("Math.pow(2, 3)"), QStringLiteral("call:pow(int:2, int:3)"));
    failures += !expectStructure(QStringLiteral("abs(-5)"), QStringLiteral("call:abs((- int:5))"));
    failures += !expectStructure(QStringLiteral("round(3.7)"), QStringLiteral("call:round(dbl:3.7)"));
    failures += !expectStructure(QStringLiteral("min(a, b)"), QStringLiteral("call:min(id:a, id:b)"));
    failures += !expectStructure(QStringLiteral("max(a, b)"), QStringLiteral("call:max(id:a, id:b)"));
    failures += !expectStructure(QStringLiteral("Math.PI"), QStringLiteral("id:Math.PI"));

    // `||` binds loosest, then `&&`, then comparison, then additive, then multiplicative, then unary.
    failures += !expectStructure(QStringLiteral("a || b && c"), QStringLiteral("(id:a || (id:b && id:c))"));
    failures += !expectStructure(QStringLiteral("a && b || c"), QStringLiteral("((id:a && id:b) || id:c)"));
    failures += !expectStructure(QStringLiteral("a && b == c"), QStringLiteral("(id:a && (id:b == id:c))"));
    failures += !expectStructure(QStringLiteral("!a == b"), QStringLiteral("((! id:a) == id:b)"));
    failures += !expectStructure(QStringLiteral("-a < -3"), QStringLiteral("((- id:a) < (- int:3))"));
    // Left-associativity at every binary level.
    failures += !expectStructure(QStringLiteral("a || b || c"), QStringLiteral("((id:a || id:b) || id:c)"));
    failures += !expectStructure(QStringLiteral("a && b && c"), QStringLiteral("((id:a && id:b) && id:c)"));
    failures += !expectStructure(QStringLiteral("a < b < c"), QStringLiteral("((id:a < id:b) < id:c)"));
    // Parentheses override precedence, and nest/collapse without trace.
    failures += !expectStructure(QStringLiteral("(a || b) && c"), QStringLiteral("((id:a || id:b) && id:c)"));
    failures += !expectStructure(QStringLiteral("((a))"), QStringLiteral("id:a"));
    failures += !expectStructure(QStringLiteral("!(a && b)"), QStringLiteral("(! (id:a && id:b))"));
    // Unary chains.
    failures += !expectStructure(QStringLiteral("!!a"), QStringLiteral("(! (! id:a))"));
    failures += !expectStructure(QStringLiteral("!!!a"), QStringLiteral("(! (! (! id:a)))"));
    failures += !expectStructure(QStringLiteral("- -3"), QStringLiteral("(- (- int:3))"));
    // Whitespace is not structure: the dense form must group identically to
    // the spaced one.
    {
        const ParseResult dense = app::expr::parse(QStringLiteral("  count>3&&!locked "));
        const ParseResult spaced = app::expr::parse(QStringLiteral("count > 3 && !locked"));
        if (!dense.ok || !spaced.ok || dump(dense.ast) != dump(spaced.ast)) {
            std::fprintf(stderr, "FAIL: whitespace changed the parse of `count>3&&!locked`: %s vs %s\n",
                         qUtf8Printable(dump(dense.ast)), qUtf8Printable(dump(spaced.ast)));
            ++failures;
        }
    }
    // Source spans, which renameIdentifierInSource() relies on: both Identifier
    // tokens and the enclosing binary node's full extent are checked.
    {
        const QString source = QStringLiteral("count > 3 && !locked");
        const ParseResult parsed = app::expr::parse(source);
        bool spansOk = parsed.ok;
        int checkedIdentifiers = 0;
        if (spansOk) {
            for (const app::expr::Node& node : parsed.ast.nodes) {
                if (node.kind != NodeKind::Identifier) {
                    continue;
                }
                ++checkedIdentifiers;
                if (source.mid(node.sourceOffset, node.sourceLength) != node.text) {
                    std::fprintf(stderr, "FAIL: identifier span (%d,%d) of \"%s\" reads \"%s\" (want \"%s\")\n",
                                 node.sourceOffset, node.sourceLength, qUtf8Printable(source),
                                 qUtf8Printable(source.mid(node.sourceOffset, node.sourceLength)),
                                 qUtf8Printable(node.text));
                    spansOk = false;
                }
            }
            const app::expr::Node& root = parsed.ast.nodes.at(static_cast<size_t>(parsed.ast.root));
            if (root.sourceOffset != 0 || root.sourceLength != static_cast<int>(source.size())) {
                std::fprintf(stderr, "FAIL: root span of \"%s\" is (%d,%d) (want (0,%d))\n", qUtf8Printable(source),
                             root.sourceOffset, root.sourceLength, static_cast<int>(source.size()));
                spansOk = false;
            }
        }
        if (checkedIdentifiers != 2) {
            std::fprintf(stderr, "FAIL: \"%s\" yielded %d Identifier node(s) (want 2)\n", qUtf8Printable(source),
                         checkedIdentifiers);
            spansOk = false;
        }
        failures += !spansOk;
    }
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 1 (parser/precedence) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 2. parse failures, each asserting the reported position ------------
    failures += !expectParseFailure(QStringLiteral("(a && b"), 7);        // unbalanced paren -> at end of input
    failures += !expectParseFailure(QStringLiteral("a &&"), 4);           // trailing operator -> at end of input
    failures += !expectParseFailure(QStringLiteral("a # b"), 2);          // unknown character -> at the character
    failures += !expectParseFailure(QStringLiteral("'admin"), 0);         // unterminated string -> at the OPENING quote
    failures += !expectParseFailure(QStringLiteral("a = b"), 2);          // single '=' is reserved for assign
    failures += !expectParseFailure(QStringLiteral("a & b"), 2);          // half-typed '&&'
    failures += !expectParseFailure(QStringLiteral("a | b"), 2);          // half-typed '||'
    failures += !expectParseFailure(QStringLiteral("()"), 1);             // empty parentheses -> expected an expression
    failures += !expectParseFailure(QStringLiteral("a b"), 2);            // two primaries -> unexpected trailing input
    failures += !expectParseFailure(QStringLiteral("a)"), 1);             // stray close paren -> trailing input
    failures += !expectParseFailure(QStringLiteral(""), 0);               // nothing at all is not an expression
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 2 (parse failures) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 3. isBareIdentifier: the compatibility rule ------------------------
    failures += !expectBareIdentifier(QStringLiteral("count"), true);
    failures += !expectBareIdentifier(QStringLiteral("isAdmin"), true);
    failures += !expectBareIdentifier(QStringLiteral("_leading"), true);
    // Surrounding whitespace must not flip a hook into an expression: the
    // classification is about the TOKEN, not the byte count.
    failures += !expectBareIdentifier(QStringLiteral("  count  "), true);
    failures += !expectBareIdentifier(QStringLiteral("a b"), false);
    failures += !expectBareIdentifier(QStringLiteral("a()"), false);
    failures += !expectBareIdentifier(QStringLiteral("!a"), false);
    failures += !expectBareIdentifier(QString(), false);
    failures += !expectBareIdentifier(QStringLiteral("   "), false);
    failures += !expectBareIdentifier(QStringLiteral("count > 3"), false);
    failures += !expectBareIdentifier(QStringLiteral("count == count"), false);
    // `true`/`false` are Bool LITERALS in grammar v1, not identifiers, so a
    // bare `true` is a one-term expression rather than a hook named "true".
    failures += !expectBareIdentifier(QStringLiteral("true"), false);
    failures += !expectBareIdentifier(QStringLiteral("false"), false);
    failures += !expectBareIdentifier(QStringLiteral("42"), false);
    failures += !expectBareIdentifier(QStringLiteral("'admin'"), false);
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 3 (isBareIdentifier) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 4. typeCheck against a four-type schema --------------------------
    const QVector<app::ContextVariable> schema = {
        variable(QStringLiteral("count"), app::ContextType::Int),
        variable(QStringLiteral("ratio"), app::ContextType::Double),
        variable(QStringLiteral("locked"), app::ContextType::Bool),
        variable(QStringLiteral("role"), app::ContextType::String),
    };

    failures += !expectTypeProblems(QStringLiteral("missing > 3"), schema, 1);        // unknown identifier
    failures += !expectTypeProblems(QStringLiteral("locked < locked"), schema, 1);    // Bool is never ordered
    failures += !expectTypeProblems(QStringLiteral("count == role"), schema, 1);      // Int vs String
    failures += !expectTypeProblems(QStringLiteral("count"), schema, 1);              // non-Bool top level
    failures += !expectTypeProblems(QStringLiteral("role"), schema, 1);               // ditto, String
    failures += !expectTypeProblems(QStringLiteral("!count"), schema, 1);             // '!' wants Bool
    failures += !expectTypeProblems(QStringLiteral("-locked"), schema, 1);            // unary '-' wants numeric
    failures += !expectTypeProblems(QStringLiteral("count && locked"), schema, 1);    // '&&' wants Bool on both sides
    failures += !expectTypeProblems(QStringLiteral("role < 3"), schema, 1);           // String vs Int

    // Arithmetic typing & promotion
    failures += !expectTypeProblems(QStringLiteral("count + 1 == 2"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("count - 1 < 10"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("count * 2 > 0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("count / 2 == 0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("count % 2 == 1"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("ratio + 1.0 > 2.0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("ratio * count > 5.0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("role + 'suffix' == 'adminsuffix'"), schema, 0);
    // Math functions
    failures += !expectTypeProblems(QStringLiteral("sqrt(ratio) > 1.0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("pow(2.0, count) > 10.0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("abs(count) == 5"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("round(ratio) == 2.0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("min(count, 10) < 10"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("max(ratio, 1.0) > 1.0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("Math.PI > 3.0"), schema, 0);

    // Ill-typed arithmetic
    failures += !expectTypeProblems(QStringLiteral("role - 1 == 0"), schema, 1);
    failures += !expectTypeProblems(QStringLiteral("role * 2 == 0"), schema, 1);
    failures += !expectTypeProblems(QStringLiteral("count % ratio == 0"), schema, 1);
    failures += !expectTypeProblems(QStringLiteral("sqrt('bad') == 0.0"), schema, 1);
    failures += !expectTypeProblems(QStringLiteral("pow(1) == 0.0"), schema, 1);
    // Two independent bad leaves report two findings, not one and not four:
    // an inspector marks each site once, with no per-operator cascade.
    failures += !expectTypeProblems(QStringLiteral("missing > 3 && absent < 1"), schema, 2);
    // Accepted rows.
    failures += !expectTypeProblems(QStringLiteral("count > ratio"), schema, 0);      // Int/Double mix promotes
    failures += !expectTypeProblems(QStringLiteral("ratio == 3"), schema, 0);         // Double vs Int literal
    failures += !expectTypeProblems(QStringLiteral("count == 3"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("-count < 0"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("role == 'admin'"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("role < 'b'"), schema, 0);         // String ordering is allowed
    failures += !expectTypeProblems(QStringLiteral("locked == true"), schema, 0);     // Bool equality is allowed
    failures += !expectTypeProblems(QStringLiteral("locked != false"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("!locked"), schema, 0);
    failures += !expectTypeProblems(QStringLiteral("locked && count > 3 || role == 'admin'"), schema, 0);
    // A Bool variable tested ALONE is not an expression guard: bare `locked` is
    // a named hook, and only `locked == true` / `!locked` reach typeCheck.
    if (!app::expr::isBareIdentifier(QStringLiteral("locked")) ||
        !app::expr::validateGuardSource(QStringLiteral("locked"), schema, nullptr).isEmpty()) {
        std::fprintf(stderr, "FAIL: bare `locked` is no longer classified as a named hook by the compatibility rule\n");
        ++failures;
    }
    // validateGuardSource's two early returns, and its position reporting.
    if (!app::expr::validateGuardSource(QString(), schema, nullptr).isEmpty()) {
        std::fprintf(stderr, "FAIL: validateGuardSource(\"\") reported a problem -- a blank guard means no guard\n");
        ++failures;
    }
    {
        int position = -1;
        const QString message = app::expr::validateGuardSource(QStringLiteral("count > "), schema, &position);
        if (message.isEmpty() || position != 8) {
            std::fprintf(stderr, "FAIL: validateGuardSource(\"count > \") = \"%s\"@%d (want a message at 8)\n",
                         qUtf8Printable(message), position);
            ++failures;
        }
    }
    {
        int position = -1;
        const QString message = app::expr::validateGuardSource(QStringLiteral("count == role"), schema, &position);
        if (message.isEmpty() || position != 0) {
            std::fprintf(stderr, "FAIL: validateGuardSource(\"count == role\") = \"%s\"@%d (want a message at 0)\n",
                         qUtf8Printable(message), position);
            ++failures;
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 4 (typeCheck) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 5. evaluate: truth tables, comparison, promotion, missing values ----
    for (int lhs = 0; lhs < 2; ++lhs) {
        for (int rhs = 0; rhs < 2; ++rhs) {
            const QVariantMap values = {{QStringLiteral("a"), lhs != 0}, {QStringLiteral("b"), rhs != 0}};
            failures += !expectBoolResult(QStringLiteral("a && b"), values, (lhs != 0) && (rhs != 0));
            failures += !expectBoolResult(QStringLiteral("a || b"), values, (lhs != 0) || (rhs != 0));
            failures += !expectBoolResult(QStringLiteral("a == b"), values, (lhs != 0) == (rhs != 0));
            failures += !expectBoolResult(QStringLiteral("a != b"), values, (lhs != 0) != (rhs != 0));
        }
        const QVariantMap values = {{QStringLiteral("a"), lhs != 0}};
        failures += !expectBoolResult(QStringLiteral("!a"), values, lhs == 0);
    }
    {
        const QVariantMap values = {{QStringLiteral("count"), 5},
                                    {QStringLiteral("ratio"), 3.5},
                                    {QStringLiteral("locked"), false},
                                    {QStringLiteral("role"), QStringLiteral("admin")}};
        failures += !expectBoolResult(QStringLiteral("count > 3"), values, true);
        failures += !expectBoolResult(QStringLiteral("count < 3"), values, false);
        failures += !expectBoolResult(QStringLiteral("count >= 5"), values, true);
        failures += !expectBoolResult(QStringLiteral("count <= 4"), values, false);
        failures += !expectBoolResult(QStringLiteral("-count < 0"), values, true);
        // Int/Double promotion in both directions.
        failures += !expectBoolResult(QStringLiteral("count > ratio"), values, true);
        failures += !expectBoolResult(QStringLiteral("ratio < count"), values, true);
        failures += !expectBoolResult(QStringLiteral("ratio > 3"), values, true);
        failures += !expectBoolResult(QStringLiteral("ratio == 3.5"), values, true);
        failures += !expectBoolResult(QStringLiteral("-ratio < 0"), values, true);

        // Arithmetic evaluation
        failures += !expectBoolResult(QStringLiteral("count + 5 == 10"), values, true);
        failures += !expectBoolResult(QStringLiteral("count - 2 == 3"), values, true);
        failures += !expectBoolResult(QStringLiteral("count * 10 == 50"), values, true);
        failures += !expectBoolResult(QStringLiteral("ratio * 2.0 == 7.0"), values, true);
        failures += !expectBoolResult(QStringLiteral("10 / 2 == 5"), values, true);
        failures += !expectBoolResult(QStringLiteral("7 % 3 == 1"), values, true);
        failures += !expectBoolResult(QStringLiteral("role + '!' == 'admin!'"), values, true);

        // Math calls evaluation
        failures += !expectBoolResult(QStringLiteral("sqrt(16.0) == 4.0"), values, true);
        failures += !expectBoolResult(QStringLiteral("Math.sqrt(16.0) == 4.0"), values, true);
        failures += !expectBoolResult(QStringLiteral("pow(2.0, 3.0) == 8.0"), values, true);
        failures += !expectBoolResult(QStringLiteral("Math.pow(2.0, 3.0) == 8.0"), values, true);
        failures += !expectBoolResult(QStringLiteral("abs(-42) == 42"), values, true);
        failures += !expectBoolResult(QStringLiteral("round(3.7) == 4.0"), values, true);
        failures += !expectBoolResult(QStringLiteral("floor(3.7) == 3.0"), values, true);
        failures += !expectBoolResult(QStringLiteral("ceil(3.2) == 4.0"), values, true);
        failures += !expectBoolResult(QStringLiteral("min(10, 20) == 10"), values, true);
        failures += !expectBoolResult(QStringLiteral("max(10, 20) == 20"), values, true);
        failures += !expectBoolResult(QStringLiteral("Math.PI > 3.14 && Math.PI < 3.15"), values, true);

        // Division by zero safely yields ok == false
        {
            bool ok = true;
            const app::expr::ParseResult divZero = app::expr::parse(QStringLiteral("count / 0 == 0"));
            app::expr::evaluate(divZero.ast, values, &ok);
            if (ok) {
                std::fprintf(stderr, "FAIL: count / 0 evaluated with ok=true (want ok=false)\n");
                ++failures;
            }
        }
        // Modulo by zero safely yields ok == false
        {
            bool ok = true;
            const app::expr::ParseResult modZero = app::expr::parse(QStringLiteral("count % 0 == 0"));
            app::expr::evaluate(modZero.ast, values, &ok);
            if (ok) {
                std::fprintf(stderr, "FAIL: count % 0 evaluated with ok=true (want ok=false)\n");
                ++failures;
            }
        }
        // Negative sqrt safely yields ok == false
        {
            bool ok = true;
            const app::expr::ParseResult negSqrt = app::expr::parse(QStringLiteral("sqrt(-1.0) == 0.0"));
            app::expr::evaluate(negSqrt.ast, values, &ok);
            if (ok) {
                std::fprintf(stderr, "FAIL: sqrt(-1.0) evaluated with ok=true (want ok=false)\n");
                ++failures;
            }
        }
        // String equality and ordering.
        failures += !expectBoolResult(QStringLiteral("role == 'admin'"), values, true);
        failures += !expectBoolResult(QStringLiteral("role == 'guest'"), values, false);
        failures += !expectBoolResult(QStringLiteral("role != 'guest'"), values, true);
        failures += !expectBoolResult(QStringLiteral("role < 'b'"), values, true);
        failures += !expectBoolResult(QStringLiteral("role >= 'admin'"), values, true);
        // Bool variable via the wart's workaround, plus a whole composed guard.
        failures += !expectBoolResult(QStringLiteral("locked == true"), values, false);
        failures += !expectBoolResult(QStringLiteral("!locked"), values, true);
        failures += !expectBoolResult(QStringLiteral("!locked && count > 3 || role == 'guest'"), values, true);
        failures += !expectBoolResult(QStringLiteral("locked && count > 3 || role == 'guest'"), values, false);
        // Literal-only expressions need no values at all.
        failures += !expectBoolResult(QStringLiteral("true"), QVariantMap(), true);
        failures += !expectBoolResult(QStringLiteral("3 < 4"), QVariantMap(), true);
    }
    // A missing value never crashes: ok comes back false. The evaluator does
    // not short-circuit, so `false && missing` still surfaces the gap.
    failures += !expectEvaluateNotOk(QStringLiteral("count > 3"), QVariantMap());
    failures += !expectEvaluateNotOk(QStringLiteral("locked && count > 3"),
                                     QVariantMap{{QStringLiteral("locked"), false}});
    failures += !expectEvaluateNotOk(QStringLiteral("locked || count > 3"),
                                     QVariantMap{{QStringLiteral("locked"), true}});
    // A value whose QVariant type is outside grammar v1's four is a gap too,
    // not a silent coercion.
    failures += !expectEvaluateNotOk(QStringLiteral("count > 3"),
                                     QVariantMap{{QStringLiteral("count"), QVariant(QPointF(1, 2))}});
    // An ill-typed tree that never went through typeCheck is refused rather
    // than guessed at.
    failures += !expectEvaluateNotOk(QStringLiteral("locked < locked"),
                                     QVariantMap{{QStringLiteral("locked"), false}});
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 5 (evaluate) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 6. print: canonical, minimal-parenthesised, fixed point ----------
    failures += !expectPrint(QStringLiteral("a||b&&c"), QStringLiteral("a || b && c"));
    failures += !expectPrint(QStringLiteral("(a||b)&&c"), QStringLiteral("(a || b) && c"));
    failures += !expectPrint(QStringLiteral("a&&b||c"), QStringLiteral("a && b || c"));
    failures += !expectPrint(QStringLiteral("a&&(b||c)"), QStringLiteral("a && (b || c)"));
    // Left-associativity means only the RIGHT side of a same-precedence pair
    // needs brackets.
    failures += !expectPrint(QStringLiteral("a||b||c"), QStringLiteral("a || b || c"));
    failures += !expectPrint(QStringLiteral("a||(b||c)"), QStringLiteral("a || (b || c)"));
    // A left-nested chain needs NO brackets (it re-parses to the same tree);
    // a right-nested one does.
    failures += !expectPrint(QStringLiteral("a<b<c"), QStringLiteral("a < b < c"));
    failures += !expectPrint(QStringLiteral("(a<b)<c"), QStringLiteral("a < b < c"));
    failures += !expectPrint(QStringLiteral("a<(b<c)"), QStringLiteral("a < (b < c)"));
    failures += !expectPrint(QStringLiteral("a&&b&&c"), QStringLiteral("a && b && c"));
    failures += !expectPrint(QStringLiteral("a&&(b&&c)"), QStringLiteral("a && (b && c)"));
    failures += !expectPrint(QStringLiteral("!(a&&b)"), QStringLiteral("!(a && b)"));
    failures += !expectPrint(QStringLiteral("!a&&b"), QStringLiteral("!a && b"));

    // Arithmetic & call canonical print
    failures += !expectPrint(QStringLiteral("a+b*c"), QStringLiteral("a + b * c"));
    failures += !expectPrint(QStringLiteral("(a+b)*c"), QStringLiteral("(a + b) * c"));
    failures += !expectPrint(QStringLiteral("pow(a,b)"), QStringLiteral("pow(a, b)"));
    failures += !expectPrint(QStringLiteral("sqrt(16.0)"), QStringLiteral("sqrt(16.0)"));
    // Redundant parentheses are dropped: print() is canonical.
    failures += !expectPrint(QStringLiteral("((a))"), QStringLiteral("a"));
    failures += !expectPrint(QStringLiteral("(count) > (3)"), QStringLiteral("count > 3"));
    // Literals re-emit in a form that re-parses to the SAME NodeKind -- a
    // Double printed as "3" would come back an Int and break the fixed point.
    failures += !expectPrint(QStringLiteral("3.5"), QStringLiteral("3.5"));
    failures += !expectPrint(QStringLiteral("3.0"), QStringLiteral("3.0"));
    failures += !expectPrint(QStringLiteral("0.1"), QStringLiteral("0.1"));
    failures += !expectPrint(QStringLiteral("-3"), QStringLiteral("-3"));
    failures += !expectPrint(QStringLiteral("'admin'"), QStringLiteral("'admin'"));
    failures += !expectPrint(QStringLiteral("true"), QStringLiteral("true"));
    failures += !expectPrint(QStringLiteral("  count>3  &&  !locked "), QStringLiteral("count > 3 && !locked"));
    failures += !expectPrint(QStringLiteral("locked==true||role<'b'&&count>=10"),
                             QStringLiteral("locked == true || role < 'b' && count >= 10"));
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 6 (print) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 7. renameIdentifierInSource: the user's own text, preserved
    // ------------------------------------------------------------------------
    // Spacing and parentheses survive byte-for-byte (spans, not a print() reprint).
    failures += !expectRename(QStringLiteral("  ( count>3 )  && !locked "), QStringLiteral("count"),
                              QStringLiteral("total"), QStringLiteral("  ( total>3 )  && !locked "));
    // Multiple occurrences, all rewritten.
    failures += !expectRename(QStringLiteral("count > 3 && count < 10"), QStringLiteral("count"),
                              QStringLiteral("total"), QStringLiteral("total > 3 && total < 10"));
    // A SHORTER replacement shifts every later span: proof the walk really is
    // right-to-left and not left-to-right with stale offsets.
    failures += !expectRename(QStringLiteral("count > 3 && count < counter"), QStringLiteral("count"),
                              QStringLiteral("c"), QStringLiteral("c > 3 && c < counter"));
    // A LONGER replacement, same reason.
    failures += !expectRename(QStringLiteral("count>0&&count<9"), QStringLiteral("count"),
                              QStringLiteral("attemptCount"), QStringLiteral("attemptCount>0&&attemptCount<9"));
    // A same-named STRING LITERAL is data, not a reference.
    failures += !expectRename(QStringLiteral("role == 'count' && count > 1"), QStringLiteral("count"),
                              QStringLiteral("total"), QStringLiteral("role == 'count' && total > 1"));
    // No partial match inside a longer identifier, in either direction.
    failures += !expectRename(QStringLiteral("counter > count"), QStringLiteral("count"), QStringLiteral("total"),
                              QStringLiteral("counter > total"));
    failures += !expectRename(QStringLiteral("mycount > 1 && count_2 > 2"), QStringLiteral("count"),
                              QStringLiteral("total"), QStringLiteral("mycount > 1 && count_2 > 2"));
    // A bare identifier is a HOOK name, so a context rename must not touch it
    // (the compatibility rule again).
    failures += !expectRename(QStringLiteral("count"), QStringLiteral("count"), QStringLiteral("total"),
                              QStringLiteral("count"));
    // Unparseable text is returned verbatim rather than corrupted.
    failures += !expectRename(QStringLiteral("count > "), QStringLiteral("count"), QStringLiteral("total"),
                              QStringLiteral("count > "));
    failures += !expectRename(QStringLiteral("count # 3"), QStringLiteral("count"), QStringLiteral("total"),
                              QStringLiteral("count # 3"));
    // No-ops stay no-ops.
    failures += !expectRename(QStringLiteral("count > 3"), QStringLiteral("count"), QStringLiteral("count"),
                              QStringLiteral("count > 3"));
    failures += !expectRename(QStringLiteral("count > 3"), QStringLiteral("absent"), QStringLiteral("total"),
                              QStringLiteral("count > 3"));
    failures += !expectRename(QStringLiteral("count > 3"), QString(), QStringLiteral("total"),
                              QStringLiteral("count > 3"));
    // The renamed text must still parse to the SAME SHAPE: a rewriter that
    // clipped a span would leave something that no longer groups the same way.
    {
        const QString source = QStringLiteral("(count > 3 || ratio <= 0.5) && !locked");
        const QString renamed = app::expr::renameIdentifierInSource(source, QStringLiteral("count"),
                                                                   QStringLiteral("attempts"));
        const ParseResult before = app::expr::parse(source);
        const ParseResult after = app::expr::parse(renamed);
        if (renamed != QStringLiteral("(attempts > 3 || ratio <= 0.5) && !locked") || !after.ok ||
            dump(after.ast) != dump(before.ast).replace(QStringLiteral("id:count"), QStringLiteral("id:attempts"))) {
            std::fprintf(stderr, "FAIL: rename of a composed guard produced \"%s\" (%s)\n", qUtf8Printable(renamed),
                         qUtf8Printable(dump(after.ast)));
            ++failures;
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 7 (renameIdentifierInSource) had %d failure(s)\n",
                     failures);
        return 1;
    }

    // ---- 7b. textReferencesIdentifier: token matching and cross-highlighting
    {
        auto expectRef = [](const QString& source, const QString& id, bool expected) {
            const bool actual = app::expr::textReferencesIdentifier(source, id);
            if (actual != expected) {
                std::fprintf(stderr, "FAIL: textReferencesIdentifier(\"%s\", \"%s\") = %d (want %d)\n",
                             qUtf8Printable(source), qUtf8Printable(id), actual ? 1 : 0, expected ? 1 : 0);
                return false;
            }
            return true;
        };
        int refFailures = 0;
        refFailures += !expectRef(QStringLiteral("x > 10"), QStringLiteral("x"), true);
        refFailures += !expectRef(QStringLiteral("targetX > 10"), QStringLiteral("x"), false);
        refFailures += !expectRef(QStringLiteral("x_val == 1"), QStringLiteral("x"), false);
        refFailures += !expectRef(QStringLiteral("maxVelocity + x"), QStringLiteral("x"), true);
        refFailures += !expectRef(QStringLiteral("x = round(y)"), QStringLiteral("x"), true);
        refFailures += !expectRef(QStringLiteral("x = round(y)"), QStringLiteral("y"), true);
        refFailures += !expectRef(QStringLiteral("x = round(y)"), QStringLiteral("z"), false);
        refFailures += !expectRef(QStringLiteral("msg == 'hello x'"), QStringLiteral("x"), false);
        refFailures += !expectRef(QStringLiteral(""), QStringLiteral("x"), false);
        refFailures += !expectRef(QStringLiteral("x"), QString(), false);
        if (refFailures != 0) {
            std::fprintf(stderr, "FAIL: expression phase section 7b (textReferencesIdentifier) had %d failure(s)\n",
                         refFailures);
            return 1;
        }
    }


    // ---- 7c. MemberAccess AST, typeCheck, and evaluate with Object Context
    {
        int memberFailures = 0;

        // Structure & print fixed point
        memberFailures += !expectStructure(QStringLiteral("user.name"), QStringLiteral("id:user.name"));
        memberFailures += !expectStructure(QStringLiteral("user.profile.age"), QStringLiteral("id:user.profile.age"));
        memberFailures += !expectStructure(QStringLiteral("user.age >= 18"), QStringLiteral("(id:user.age >= int:18)"));
        memberFailures += !expectStructure(QStringLiteral("user.active && count > 0"),
                                           QStringLiteral("(id:user.active && (id:count > int:0))"));

        memberFailures += !expectPrint(QStringLiteral("user.name"), QStringLiteral("user.name"));
        memberFailures += !expectPrint(QStringLiteral("user.profile.age"), QStringLiteral("user.profile.age"));
        memberFailures += !expectPrint(QStringLiteral("user.age >= 18"), QStringLiteral("user.age >= 18"));
        memberFailures += !expectPrint(QStringLiteral("user.active && count > 0"), QStringLiteral("user.active && count > 0"));

        memberFailures += !expectParseFailure(QStringLiteral("user."), 5);
        memberFailures += !expectParseFailure(QStringLiteral("user..age"), 5);

        // typeCheck against schema with Object variable
        app::ContextVariable userVar;
        userVar.name = QStringLiteral("user");
        userVar.type = app::ContextType::Object;
        userVar.initialValue = QStringLiteral("{\"name\": \"Alice\", \"age\": 30, \"active\": true, \"profile\": {\"score\": 98.5}}");

        app::ContextVariable countVar;
        countVar.name = QStringLiteral("count");
        countVar.type = app::ContextType::Int;
        countVar.initialValue = QStringLiteral("10");

        app::ContextVariable badVar;
        badVar.name = QStringLiteral("badJson");
        badVar.type = app::ContextType::Object;
        badVar.initialValue = QStringLiteral("{not valid json}");

        const QVector<app::ContextVariable> objectSchema = {userVar, countVar, badVar};

        memberFailures += !expectTypeProblems(QStringLiteral("user.name == 'Alice'"), objectSchema, 0);
        memberFailures += !expectTypeProblems(QStringLiteral("user.age >= 18"), objectSchema, 0);
        memberFailures += !expectTypeProblems(QStringLiteral("user.active"), objectSchema, 0);
        memberFailures += !expectTypeProblems(QStringLiteral("user.profile.score > 90.0"), objectSchema, 0);
        memberFailures += !expectTypeProblems(QStringLiteral("user.unknownProp == 1"), objectSchema, 1);
        memberFailures += !expectTypeProblems(QStringLiteral("user.profile.missingScore > 0"), objectSchema, 1);
        memberFailures += !expectTypeProblems(QStringLiteral("count.prop == 1"), objectSchema, 1);
        memberFailures += !expectTypeProblems(QStringLiteral("badJson.prop == 1"), objectSchema, 1);

        // evaluate against QVariantMap containing nested Object
        QVariantMap userProfile;
        userProfile[QStringLiteral("score")] = 98.5;
        QVariantMap userObj;
        userObj[QStringLiteral("name")] = QStringLiteral("Alice");
        userObj[QStringLiteral("age")] = 30;
        userObj[QStringLiteral("active")] = true;
        userObj[QStringLiteral("profile")] = userProfile;

        QVariantMap evalContext;
        evalContext[QStringLiteral("user")] = userObj;
        evalContext[QStringLiteral("count")] = 10;

        memberFailures += !expectBoolResult(QStringLiteral("user.name == 'Alice'"), evalContext, true);
        memberFailures += !expectBoolResult(QStringLiteral("user.age == 30"), evalContext, true);
        memberFailures += !expectBoolResult(QStringLiteral("user.active"), evalContext, true);
        memberFailures += !expectBoolResult(QStringLiteral("user.profile.score > 90.0"), evalContext, true);
        memberFailures += !expectBoolResult(QStringLiteral("user.age + count == 40"), evalContext, true);
        memberFailures += !expectEvaluateNotOk(QStringLiteral("user.missingProp == 1"), evalContext);

        // renameIdentifierInSource & textReferencesIdentifier
        memberFailures += !expectRename(QStringLiteral("user.age >= 18"), QStringLiteral("user"),
                                       QStringLiteral("account"), QStringLiteral("account.age >= 18"));
        memberFailures += !expectRename(QStringLiteral("user.age >= 18"), QStringLiteral("age"),
                                       QStringLiteral("years"), QStringLiteral("user.age >= 18"));

        if (memberFailures != 0) {
            std::fprintf(stderr, "FAIL: expression phase section 7c (member access) had %d failure(s)\n",
                         memberFailures);
            return 1;
        }
    }

    // ---- 7d. Typed Event Payload and Struct Context MemberAccess
    {
        int structFailures = 0;

        // 1. Struct definitions
        app::StructDefinition headerDef{
            .id = 1,
            .name = QStringLiteral("CanHeader"),
            .fields = {
                app::StructField{.name = QStringLiteral("timestamp"), .type = app::FieldType::Int},
                app::StructField{.name = QStringLiteral("priority"), .type = app::FieldType::Int},
            },
        };
        app::StructDefinition canMsgDef{
            .id = 2,
            .name = QStringLiteral("CanMessage"),
            .fields = {
                app::StructField{.name = QStringLiteral("id"), .type = app::FieldType::Int},
                app::StructField{.name = QStringLiteral("dlc"), .type = app::FieldType::Int},
                app::StructField{.name = QStringLiteral("extended"), .type = app::FieldType::Bool},
                app::StructField{.name = QStringLiteral("header"), .type = app::FieldType::Custom, .customTypeName = QStringLiteral("CanHeader")},
            },
        };
        const QVector<app::StructDefinition> structDefs = {headerDef, canMsgDef};

        // 2. PayloadBinding::forEvent creation
        const app::expr::PayloadBinding noneBinding = app::expr::PayloadBinding::forEvent(QStringLiteral(""), structDefs);
        if (noneBinding.kind != app::expr::PayloadKind::None || !noneBinding.name.isEmpty()) {
            std::fprintf(stderr, "FAIL: forEvent with empty payloadType should produce PayloadKind::None\n");
            structFailures++;
        }

        const app::expr::PayloadBinding intBinding = app::expr::PayloadBinding::forEvent(QStringLiteral("Int"), structDefs);
        if (intBinding.kind != app::expr::PayloadKind::Event || intBinding.name != QLatin1String("event") || intBinding.type != app::expr::ValueType::Int) {
            std::fprintf(stderr, "FAIL: forEvent(\"Int\") did not produce expected scalar binding\n");
            structFailures++;
        }

        const app::expr::PayloadBinding structBinding = app::expr::PayloadBinding::forEvent(QStringLiteral("CanMessage"), structDefs);
        if (structBinding.kind != app::expr::PayloadKind::Event || structBinding.structTypeName != QLatin1String("CanMessage") || structBinding.structDef == nullptr) {
            std::fprintf(stderr, "FAIL: forEvent(\"CanMessage\") did not bind structDef\n");
            structFailures++;
        }

        // 3. Type checking event.<field> and event.payload.<field>
        const QVector<app::ContextVariable> emptySchema;
        auto checkGuard = [&](const QString& src, const app::expr::PayloadBinding& pb, int expectedProblems) {
            const app::expr::ParseResult pr = app::expr::parse(src);
            if (!pr.ok) {
                std::fprintf(stderr, "FAIL: parse(\"%s\") failed: %s\n", qUtf8Printable(src), qUtf8Printable(pr.message));
                structFailures++;
                return;
            }
            const QVector<app::expr::TypeProblem> probs = app::expr::typeCheck(pr.ast, emptySchema, pb, structDefs);
            if (probs.size() != expectedProblems) {
                std::fprintf(stderr, "FAIL: typeCheck(\"%s\") reported %d problems (want %d)\n",
                             qUtf8Printable(src), probs.size(), expectedProblems);
                structFailures++;
            }
        };

        // Valid struct payload guards: event.<field> and event.payload.<field>
        checkGuard(QStringLiteral("event.id == 256 && event.dlc >= 8"), structBinding, 0);
        checkGuard(QStringLiteral("event.payload.id == 256"), structBinding, 0);
        checkGuard(QStringLiteral("event.extended == true"), structBinding, 0);
        checkGuard(QStringLiteral("event.header.timestamp > 0 && event.header.priority == 1"), structBinding, 0);
        checkGuard(QStringLiteral("event.payload.header.timestamp > 0"), structBinding, 0);

        // Invalid struct payload guards: unknown fields / non-nested struct navigation
        checkGuard(QStringLiteral("event.unknownField == 1"), structBinding, 1);
        checkGuard(QStringLiteral("event.payload.unknownField == 1"), structBinding, 1);
        checkGuard(QStringLiteral("event.id.nested == 1"), structBinding, 1);

        // Valid scalar payload guards
        checkGuard(QStringLiteral("event == 42"), intBinding, 0);
        checkGuard(QStringLiteral("event.payload == 42"), intBinding, 0);
        checkGuard(QStringLiteral("event.badProp == 42"), intBinding, 1);

        // 4. Type checking context variable of struct type
        app::ContextVariable structVar;
        structVar.name = QStringLiteral("lastMsg");
        structVar.type = app::ContextType::Object;
        structVar.customTypeName = QStringLiteral("CanMessage");
        const QVector<app::ContextVariable> customSchema = {structVar};

        auto checkCustomContext = [&](const QString& src, int expectedProblems) {
            const app::expr::ParseResult pr = app::expr::parse(src);
            if (!pr.ok) {
                std::fprintf(stderr, "FAIL: parse(\"%s\") failed: %s\n", qUtf8Printable(src), qUtf8Printable(pr.message));
                structFailures++;
                return;
            }
            const QVector<app::expr::TypeProblem> probs = app::expr::typeCheck(pr.ast, customSchema, app::expr::PayloadBinding::none(), structDefs);
            if (probs.size() != expectedProblems) {
                std::fprintf(stderr, "FAIL: typeCheck(\"%s\") with customSchema reported %d problems (want %d)\n",
                             qUtf8Printable(src), probs.size(), expectedProblems);
                structFailures++;
            }
        };

        checkCustomContext(QStringLiteral("lastMsg.id == 256"), 0);
        checkCustomContext(QStringLiteral("lastMsg.header.timestamp > 1000"), 0);
        checkCustomContext(QStringLiteral("lastMsg.nonExistent == 1"), 1);

        // 5. Evaluation of event.<field> and event.payload.<field>
        QVariantMap headerMap;
        headerMap[QStringLiteral("timestamp")] = 12345;
        headerMap[QStringLiteral("priority")] = 2;

        QVariantMap canMsgMap;
        canMsgMap[QStringLiteral("id")] = 0x100;
        canMsgMap[QStringLiteral("dlc")] = 8;
        canMsgMap[QStringLiteral("extended")] = true;
        canMsgMap[QStringLiteral("header")] = headerMap;

        QVariantMap simValues;
        simValues[QStringLiteral("event")] = canMsgMap;
        simValues[QStringLiteral("lastMsg")] = canMsgMap;

        auto evalBool = [&](const QString& src, bool expected) {
            const app::expr::ParseResult pr = app::expr::parse(src);
            if (!pr.ok) {
                std::fprintf(stderr, "FAIL: parse(\"%s\") failed: %s\n", qUtf8Printable(src), qUtf8Printable(pr.message));
                structFailures++;
                return;
            }
            bool ok = false;
            const app::expr::Value v = app::expr::evaluate(pr.ast, simValues, &ok);
            if (!ok || v.type != app::expr::ValueType::Bool || v.boolValue != expected) {
                std::fprintf(stderr, "FAIL: evaluate(\"%s\") = %s (ok=%d), want %s\n",
                             qUtf8Printable(src), v.boolValue ? "true" : "false", ok ? 1 : 0, expected ? "true" : "false");
                structFailures++;
            }
        };

        evalBool(QStringLiteral("event.id == 256"), true);
        evalBool(QStringLiteral("event.payload.id == 256"), true);
        evalBool(QStringLiteral("event.dlc == 8 && event.extended"), true);
        evalBool(QStringLiteral("event.header.timestamp == 12345"), true);
        evalBool(QStringLiteral("event.payload.header.timestamp == 12345"), true);
        evalBool(QStringLiteral("lastMsg.id == 256"), true);
        evalBool(QStringLiteral("lastMsg.header.priority == 2"), true);

        // Scalar payload evaluation
        QVariantMap scalarSimValues;
        scalarSimValues[QStringLiteral("event")] = 42;
        auto evalScalarBool = [&](const QString& src, bool expected) {
            const app::expr::ParseResult pr = app::expr::parse(src);
            bool ok = false;
            const app::expr::Value v = app::expr::evaluate(pr.ast, scalarSimValues, &ok);
            if (!ok || v.type != app::expr::ValueType::Bool || v.boolValue != expected) {
                std::fprintf(stderr, "FAIL: scalar evaluate(\"%s\") = %s (ok=%d), want %s\n",
                             qUtf8Printable(src), v.boolValue ? "true" : "false", ok ? 1 : 0, expected ? "true" : "false");
                structFailures++;
            }
        };
        evalScalarBool(QStringLiteral("event == 42"), true);
        evalScalarBool(QStringLiteral("event.payload == 42"), true);
        evalScalarBool(QStringLiteral("event.payload > 50"), false);

        if (structFailures != 0) {
            std::fprintf(stderr, "FAIL: expression phase section 7d (struct context & typed event) had %d failure(s)\n",
                         structFailures);
            return 1;
        }
    }

    // Phase section 8: Native Time Trigger and Composite Transition Label parsing
    {
        int timeFailures = 0;

        // 1. parseTimeTrigger positive cases
        const auto tt1 = app::expr::parseTimeTrigger(QStringLiteral("after 10s"));
        if (!tt1.ok || tt1.kind != app::expr::TimeTriggerKind::After || tt1.durationMs != 10000) {
            std::fprintf(stderr, "FAIL: parseTimeTrigger('after 10s') failed: ok=%d, ms=%d\n", tt1.ok, tt1.durationMs);
            timeFailures++;
        }

        const auto tt2 = app::expr::parseTimeTrigger(QStringLiteral("every 500ms"));
        if (!tt2.ok || tt2.kind != app::expr::TimeTriggerKind::Every || tt2.durationMs != 500) {
            std::fprintf(stderr, "FAIL: parseTimeTrigger('every 500ms') failed: ok=%d, ms=%d\n", tt2.ok, tt2.durationMs);
            timeFailures++;
        }

        const auto tt3 = app::expr::parseTimeTrigger(QStringLiteral("after 1.5s"));
        if (!tt3.ok || tt3.durationMs != 1500) {
            std::fprintf(stderr, "FAIL: parseTimeTrigger('after 1.5s') failed: ok=%d, ms=%d\n", tt3.ok, tt3.durationMs);
            timeFailures++;
        }

        const auto tt4 = app::expr::parseTimeTrigger(QStringLiteral("every 2m"));
        if (!tt4.ok || tt4.durationMs != 120000) {
            std::fprintf(stderr, "FAIL: parseTimeTrigger('every 2m') failed: ok=%d, ms=%d\n", tt4.ok, tt4.durationMs);
            timeFailures++;
        }

        const auto tt5 = app::expr::parseTimeTrigger(QStringLiteral("(after 250 ms)"));
        if (!tt5.ok || tt5.durationMs != 250) {
            std::fprintf(stderr, "FAIL: parseTimeTrigger('(after 250 ms)') failed: ok=%d, ms=%d\n", tt5.ok, tt5.durationMs);
            timeFailures++;
        }

        // 2. parseTimeTrigger negative / fallback cases
        const auto ttn1 = app::expr::parseTimeTrigger(QStringLiteral("aftermath"));
        if (ttn1.ok) {
            std::fprintf(stderr, "FAIL: parseTimeTrigger('aftermath') should not be recognized as time trigger\n");
            timeFailures++;
        }

        const auto ttn2 = app::expr::parseTimeTrigger(QStringLiteral("everyday"));
        if (ttn2.ok) {
            std::fprintf(stderr, "FAIL: parseTimeTrigger('everyday') should not be recognized as time trigger\n");
            timeFailures++;
        }

        const auto ttn3 = app::expr::parseTimeTrigger(QStringLiteral("after"));
        if (ttn3.ok) {
            std::fprintf(stderr, "FAIL: parseTimeTrigger('after') with no duration should fail\n");
            timeFailures++;
        }

        // 3. parseTransitionLabel cases
        const auto l1 = app::expr::parseTransitionLabel(QStringLiteral("CLICK [x > 0] / count = count + 1"));
        if (!l1.ok || l1.event != QStringLiteral("CLICK") || l1.guard != QStringLiteral("x > 0") ||
            l1.action != QStringLiteral("count = count + 1")) {
            std::fprintf(stderr, "FAIL: parseTransitionLabel full composite failed\n");
            timeFailures++;
        }

        const auto l2 = app::expr::parseTransitionLabel(QStringLiteral("after 500ms [isReady] / log()"));
        if (!l2.ok || l2.event != QStringLiteral("after 500ms") || !l2.timeTrigger.ok ||
            l2.timeTrigger.durationMs != 500 || l2.guard != QStringLiteral("isReady") ||
            l2.action != QStringLiteral("log()")) {
            std::fprintf(stderr, "FAIL: parseTransitionLabel time trigger composite failed\n");
            timeFailures++;
        }

        const auto l3 = app::expr::parseTransitionLabel(QStringLiteral("always [canGo]"));
        if (!l3.ok || !l3.always || l3.guard != QStringLiteral("canGo")) {
            std::fprintf(stderr, "FAIL: parseTransitionLabel always composite failed\n");
            timeFailures++;
        }

        if (timeFailures != 0) {
            std::fprintf(stderr, "FAIL: expression phase section 8 (native time triggers) had %d failure(s)\n",
                         timeFailures);
            return 1;
        }
    }

    if (failures != 0) {
        return 1;
    }

    return runExpressionIntegrationSmoke();
}
