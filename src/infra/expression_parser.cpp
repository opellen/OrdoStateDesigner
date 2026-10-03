#include "infra/expression.h"

#include <QChar>
#include <QVector>

#include <algorithm>

namespace app::expr {

namespace {

// ---- tokens ---------------------------------------------------------------

// `True`/`False` are not Identifiers, so isBareIdentifier() never treats a bare
// `true` as a hook name.
enum class TokenKind {
    End,
    Identifier,
    IntLiteral,
    DoubleLiteral,
    StringLiteral,
    True,
    False,
    LParen,
    RParen,
    Not,
    Minus,
    AndAnd,
    OrOr,
    EqualEqual,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Plus,
    Star,
    Slash,
    Percent,
    Comma,
    Dot
};

struct Token {
    TokenKind kind = TokenKind::End;
    int offset = 0;  // character offset in the ORIGINAL source
    int length = 0;
    QString text;              // Identifier name / StringLiteral value (unquoted)
    qint64 intValue = 0;
    double doubleValue = 0.0;
};

struct TokenizeResult {
    bool ok = false;
    QVector<Token> tokens;  // always terminated by one End token when ok
    QString message;
    int position = 0;
};

bool isIdentifierStart(QChar c) {
    return c == QLatin1Char('_') || (c.unicode() < 128 && c.isLetter());
}

bool isIdentifierPart(QChar c) {
    return c == QLatin1Char('_') || (c.unicode() < 128 && (c.isLetter() || c.isDigit()));
}

bool isAsciiDigit(QChar c) {
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

// Single pass, one character of lookahead. isBareIdentifier() and parse() both
// use this, so they share one definition of a token.
TokenizeResult tokenize(const QString& source) {
    TokenizeResult result;
    const int size = static_cast<int>(source.size());
    int i = 0;
    while (i < size) {
        const QChar c = source.at(i);

        if (c.isSpace()) {
            ++i;
            continue;
        }

        Token token;
        token.offset = i;

        if (isIdentifierStart(c)) {
            int start = i;
            while (i < size && isIdentifierPart(source.at(i))) {
                ++i;
            }
            token.length = i - start;
            token.text = source.mid(start, token.length);
            if (token.text == QLatin1String("true")) {
                token.kind = TokenKind::True;
            } else if (token.text == QLatin1String("false")) {
                token.kind = TokenKind::False;
            } else {
                token.kind = TokenKind::Identifier;
            }
            result.tokens.push_back(token);
            continue;
        }

        if (isAsciiDigit(c)) {
            const int start = i;
            while (i < size && isAsciiDigit(source.at(i))) {
                ++i;
            }
            bool isDouble = false;
            // A '.' starts a fraction only when a digit follows it.
            if (i + 1 < size && source.at(i) == QLatin1Char('.') && isAsciiDigit(source.at(i + 1))) {
                isDouble = true;
                ++i;
                while (i < size && isAsciiDigit(source.at(i))) {
                    ++i;
                }
            }
            // Exponent form lets print() round-trip very large or small doubles.
            if (i < size && (source.at(i) == QLatin1Char('e') || source.at(i) == QLatin1Char('E'))) {
                int probe = i + 1;
                if (probe < size && (source.at(probe) == QLatin1Char('+') || source.at(probe) == QLatin1Char('-'))) {
                    ++probe;
                }
                if (probe < size && isAsciiDigit(source.at(probe))) {
                    isDouble = true;
                    i = probe;
                    while (i < size && isAsciiDigit(source.at(i))) {
                        ++i;
                    }
                }
            }
            token.length = i - start;
            const QString literal = source.mid(start, token.length);
            if (isDouble) {
                bool converted = false;
                token.kind = TokenKind::DoubleLiteral;
                token.doubleValue = literal.toDouble(&converted);
                if (!converted) {
                    result.message = QStringLiteral("number literal '%1' is out of range").arg(literal);
                    result.position = start;
                    return result;
                }
            } else {
                bool converted = false;
                token.kind = TokenKind::IntLiteral;
                token.intValue = literal.toLongLong(&converted);
                if (!converted) {
                    result.message = QStringLiteral("integer literal '%1' is out of range").arg(literal);
                    result.position = start;
                    return result;
                }
            }
            result.tokens.push_back(token);
            continue;
        }

        // Single quotes only, no escapes (guards live inside JSON), so a string
        // cannot contain an apostrophe.
        if (c == QLatin1Char('\'')) {
            const int start = i;
            ++i;
            QString value;
            bool terminated = false;
            while (i < size) {
                if (source.at(i) == QLatin1Char('\'')) {
                    terminated = true;
                    ++i;
                    break;
                }
                value.append(source.at(i));
                ++i;
            }
            if (!terminated) {
                // Reported at the opening quote, the one to fix.
                result.message = QStringLiteral("unterminated string literal");
                result.position = start;
                return result;
            }
            token.kind = TokenKind::StringLiteral;
            token.length = i - start;
            token.text = value;
            result.tokens.push_back(token);
            continue;
        }

        const QChar next = (i + 1 < size) ? source.at(i + 1) : QChar();
        const bool nextIsEqual = (next == QLatin1Char('='));
        switch (c.unicode()) {
            case '(':
                token.kind = TokenKind::LParen;
                token.length = 1;
                break;
            case ')':
                token.kind = TokenKind::RParen;
                token.length = 1;
                break;
            case '+':
                token.kind = TokenKind::Plus;
                token.length = 1;
                break;
            case '-':
                token.kind = TokenKind::Minus;
                token.length = 1;
                break;
            case '*':
                token.kind = TokenKind::Star;
                token.length = 1;
                break;
            case '/':
                token.kind = TokenKind::Slash;
                token.length = 1;
                break;
            case '%':
                token.kind = TokenKind::Percent;
                token.length = 1;
                break;
            case ',':
                token.kind = TokenKind::Comma;
                token.length = 1;
                break;
            case '.':
                token.kind = TokenKind::Dot;
                token.length = 1;
                break;
            case '!':
                token.kind = nextIsEqual ? TokenKind::NotEqual : TokenKind::Not;
                token.length = nextIsEqual ? 2 : 1;
                break;
            case '<':
                token.kind = nextIsEqual ? TokenKind::LessEqual : TokenKind::Less;
                token.length = nextIsEqual ? 2 : 1;
                break;
            case '>':
                token.kind = nextIsEqual ? TokenKind::GreaterEqual : TokenKind::Greater;
                token.length = nextIsEqual ? 2 : 1;
                break;
            case '=':
                // A lone '=' belongs to the assign form, never a comparison.
                if (!nextIsEqual) {
                    result.message = QStringLiteral("'=' is not an operator -- did you mean '=='?");
                    result.position = i;
                    return result;
                }
                token.kind = TokenKind::EqualEqual;
                token.length = 2;
                break;
            case '&':
                if (next != QLatin1Char('&')) {
                    result.message = QStringLiteral("'&' is not an operator -- did you mean '&&'?");
                    result.position = i;
                    return result;
                }
                token.kind = TokenKind::AndAnd;
                token.length = 2;
                break;
            case '|':
                if (next != QLatin1Char('|')) {
                    result.message = QStringLiteral("'|' is not an operator -- did you mean '||'?");
                    result.position = i;
                    return result;
                }
                token.kind = TokenKind::OrOr;
                token.length = 2;
                break;
            default:
                result.message = QStringLiteral("unexpected character '%1'").arg(c);
                result.position = i;
                return result;
        }
        i += token.length;
        result.tokens.push_back(token);
    }

    Token end;
    end.kind = TokenKind::End;
    end.offset = size;
    end.length = 0;
    result.tokens.push_back(end);
    result.ok = true;
    return result;
}

// ---- parser ---------------------------------------------------------------

// Recursive descent, one function per precedence level, lowest binding first.
// Failure is carried in `failed_`, never thrown.
class Parser {
public:
    explicit Parser(const QVector<Token>& tokens) : tokens_(tokens) {}

    ParseResult run() {
        ParseResult result;
        const int root = parseOr();
        if (failed_) {
            result.message = message_;
            result.position = position_;
            return result;
        }
        if (peek().kind != TokenKind::End) {
            result.message = QStringLiteral("unexpected trailing input");
            result.position = peek().offset;
            return result;
        }
        result.ok = true;
        result.ast.nodes = std::move(nodes_);
        result.ast.root = root;
        return result;
    }

private:
    const Token& peek() const { return tokens_.at(index_); }

    void advance() {
        if (tokens_.at(index_).kind != TokenKind::End) {
            ++index_;
        }
    }

    int fail(const QString& message, int position) {
        if (!failed_) {
            failed_ = true;
            message_ = message;
            position_ = position;
        }
        return -1;
    }

    int addNode(Node node) {
        nodes_.push_back(std::move(node));
        return static_cast<int>(nodes_.size()) - 1;
    }

    // A binary node spans from its left child's start to its right child's end.
    int makeBinary(NodeKind kind, int lhs, int rhs) {
        if (lhs < 0 || rhs < 0) {
            return -1;
        }
        Node node;
        node.kind = kind;
        node.lhs = lhs;
        node.rhs = rhs;
        const Node& left = nodes_.at(static_cast<size_t>(lhs));
        const Node& right = nodes_.at(static_cast<size_t>(rhs));
        node.sourceOffset = left.sourceOffset;
        node.sourceLength = (right.sourceOffset + right.sourceLength) - left.sourceOffset;
        return addNode(std::move(node));
    }

    int parseOr() {
        int lhs = parseAnd();
        while (!failed_ && peek().kind == TokenKind::OrOr) {
            advance();
            const int rhs = parseAnd();
            lhs = makeBinary(NodeKind::Or, lhs, rhs);
        }
        return failed_ ? -1 : lhs;
    }

    int parseAnd() {
        int lhs = parseComparison();
        while (!failed_ && peek().kind == TokenKind::AndAnd) {
            advance();
            const int rhs = parseComparison();
            lhs = makeBinary(NodeKind::And, lhs, rhs);
        }
        return failed_ ? -1 : lhs;
    }

    // Left-associative: `a < b < c` parses as `(a < b) < c`, which typeCheck()
    // then rejects.
    int parseComparison() {
        int lhs = parseAdditive();
        while (!failed_) {
            NodeKind kind{};
            switch (peek().kind) {
                case TokenKind::EqualEqual: kind = NodeKind::Equal; break;
                case TokenKind::NotEqual: kind = NodeKind::NotEqual; break;
                case TokenKind::Less: kind = NodeKind::Less; break;
                case TokenKind::LessEqual: kind = NodeKind::LessEqual; break;
                case TokenKind::Greater: kind = NodeKind::Greater; break;
                case TokenKind::GreaterEqual: kind = NodeKind::GreaterEqual; break;
                default: return lhs;
            }
            advance();
            const int rhs = parseAdditive();
            lhs = makeBinary(kind, lhs, rhs);
        }
        return -1;
    }

    int parseAdditive() {
        int lhs = parseMultiplicative();
        while (!failed_ && (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus)) {
            const NodeKind kind = (peek().kind == TokenKind::Plus) ? NodeKind::Add : NodeKind::Subtract;
            advance();
            const int rhs = parseMultiplicative();
            lhs = makeBinary(kind, lhs, rhs);
        }
        return failed_ ? -1 : lhs;
    }

    int parseMultiplicative() {
        int lhs = parseUnary();
        while (!failed_ && (peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash || peek().kind == TokenKind::Percent)) {
            const NodeKind kind = (peek().kind == TokenKind::Star) ? NodeKind::Multiply
                                : (peek().kind == TokenKind::Slash) ? NodeKind::Divide
                                : NodeKind::Modulo;
            advance();
            const int rhs = parseUnary();
            lhs = makeBinary(kind, lhs, rhs);
        }
        return failed_ ? -1 : lhs;
    }

    int parseUnary() {
        const TokenKind kind = peek().kind;
        if (kind == TokenKind::Plus) {
            advance();
            return parseUnary();
        }
        if (kind == TokenKind::Not || kind == TokenKind::Minus) {
            const Token op = peek();
            advance();
            const int operand = parseUnary();
            if (operand < 0) {
                return -1;
            }
            Node node;
            node.kind = (kind == TokenKind::Not) ? NodeKind::Not : NodeKind::Negate;
            node.lhs = operand;
            const Node& child = nodes_.at(static_cast<size_t>(operand));
            node.sourceOffset = op.offset;
            node.sourceLength = (child.sourceOffset + child.sourceLength) - op.offset;
            return addNode(std::move(node));
        }
        return parsePostfix();
    }

    int parsePostfix() {
        int lhs = parsePrimary();
        while (!failed_) {
            if (peek().kind == TokenKind::Dot) {
                advance();
                if (peek().kind != TokenKind::Identifier) {
                    return fail(QStringLiteral("expected identifier after '.'"), peek().offset);
                }
                const Token memberToken = peek();
                advance();
                Node node;
                node.kind = NodeKind::MemberAccess;
                node.lhs = lhs;
                node.text = memberToken.text;
                node.sourceOffset = nodes_.at(static_cast<size_t>(lhs)).sourceOffset;
                node.sourceLength = (memberToken.offset + memberToken.length) - node.sourceOffset;
                lhs = addNode(std::move(node));
            } else if (peek().kind == TokenKind::LParen) {
                const Node& calleeNode = nodes_.at(static_cast<size_t>(lhs));
                if (calleeNode.kind != NodeKind::Identifier && calleeNode.kind != NodeKind::MemberAccess) {
                    return fail(QStringLiteral("expression cannot be called as a function"), peek().offset);
                }
                // Capture callee attributes BEFORE parseOr() reallocates nodes_
                QString fnName;
                const int calleeOffset = calleeNode.sourceOffset;
                if (calleeNode.kind == NodeKind::Identifier) {
                    fnName = calleeNode.text;
                } else if (calleeNode.kind == NodeKind::MemberAccess) {
                    const Node& ownerNode = nodes_.at(static_cast<size_t>(calleeNode.lhs));
                    if (ownerNode.kind == NodeKind::Identifier && ownerNode.text == QLatin1String("Math")) {
                        fnName = calleeNode.text;
                    } else {
                        fnName = ownerNode.text + QLatin1Char('.') + calleeNode.text;
                    }
                }

                advance(); // consume '('
                std::vector<int> args;
                if (peek().kind != TokenKind::RParen) {
                    while (!failed_) {
                        const int arg = parseOr();
                        if (arg < 0) return -1;
                        args.push_back(arg);
                        if (peek().kind == TokenKind::Comma) {
                            advance();
                        } else {
                            break;
                        }
                    }
                }
                if (peek().kind != TokenKind::RParen) {
                    return fail(QStringLiteral("expected ')' after arguments"), peek().offset);
                }
                const Token rparen = peek();
                advance();

                Node callNode;
                callNode.kind = NodeKind::Call;
                callNode.text = fnName;
                callNode.lhs = args.size() > 0 ? args[0] : -1;
                callNode.rhs = args.size() > 1 ? args[1] : -1;
                for (size_t a = 2; a < args.size(); ++a) {
                    callNode.extraArgs.push_back(args[a]);
                }
                callNode.sourceOffset = calleeOffset;
                callNode.sourceLength = (rparen.offset + rparen.length) - calleeOffset;
                lhs = addNode(std::move(callNode));
            } else {
                break;
            }
        }
        return lhs;
    }

    int parsePrimary() {
        const Token token = peek();
        Node node;
        node.sourceOffset = token.offset;
        node.sourceLength = token.length;
        switch (token.kind) {
            case TokenKind::True:
            case TokenKind::False:
                node.kind = NodeKind::BoolLiteral;
                node.boolValue = (token.kind == TokenKind::True);
                advance();
                return addNode(std::move(node));
            case TokenKind::IntLiteral:
                node.kind = NodeKind::IntLiteral;
                node.intValue = token.intValue;
                advance();
                return addNode(std::move(node));
            case TokenKind::DoubleLiteral:
                node.kind = NodeKind::DoubleLiteral;
                node.doubleValue = token.doubleValue;
                advance();
                return addNode(std::move(node));
            case TokenKind::StringLiteral:
                node.kind = NodeKind::StringLiteral;
                node.text = token.text;
                advance();
                return addNode(std::move(node));
            case TokenKind::Identifier:
                node.kind = NodeKind::Identifier;
                node.text = token.text;
                advance();
                return addNode(std::move(node));
            case TokenKind::LParen: {
                advance();
                const int inner = parseOr();
                if (inner < 0) {
                    return -1;
                }
                if (peek().kind != TokenKind::RParen) {
                    return fail(QStringLiteral("expected ')'"), peek().offset);
                }
                advance();
                // Parentheses stay out of `inner`'s span, or a rename would
                // splice over them (`(count)` -> `total`).
                return inner;
            }
            default:
                return fail(QStringLiteral("expected an expression"), token.offset);
        }
    }

    const QVector<Token>& tokens_;
    std::vector<Node> nodes_;
    int index_ = 0;
    bool failed_ = false;
    QString message_;
    int position_ = 0;
};

}  // namespace

// ---- entry points --------------------------------------------------------

bool isBareIdentifier(const QString& source) {
    // Via the tokenizer, so surrounding whitespace is ignored and `true`/`false`
    // are literals, not hooks.
    const TokenizeResult tokens = tokenize(source);
    if (!tokens.ok || tokens.tokens.size() != 2) {
        return false;
    }
    return tokens.tokens.at(0).kind == TokenKind::Identifier;
}

ParseResult parse(const QString& source) {
    const TokenizeResult tokens = tokenize(source);
    if (!tokens.ok) {
        ParseResult result;
        result.message = tokens.message;
        result.position = tokens.position;
        return result;
    }
    Parser parser(tokens.tokens);
    return parser.run();
}

}  // namespace app::expr
