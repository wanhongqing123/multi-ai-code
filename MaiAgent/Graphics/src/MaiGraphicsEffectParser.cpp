#include "MaiGraphicsEffectParser.h"

#include <cctype>
#include <utility>

namespace {

enum class TokenKind { Word, String, Symbol, End };

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    std::size_t line = 1;
    std::size_t column = 1;
};

class EffectReader {
public:
    explicit EffectReader(const std::string& source) : mSource(source) {}

    MaiGraphicsEffectParseResult read() {
        if (!tokenize()) return mResult;
        while (!atEnd() && mResult.error.empty()) {
            if (current().text == "#") {
                readDirective();
            } else if (current().text == "technique") {
                readTechnique();
            } else {
                skipDeclaration();
            }
        }
        if (mResult.error.empty() && mResult.effect.techniques.empty())
            fail(current(), "effect requires at least one technique");
        if (!mResult.error.empty()) mResult.effect = {};
        return mResult;
    }

private:
    static bool isWordStart(char character) {
        const unsigned char value = static_cast<unsigned char>(character);
        return std::isalpha(value) || character == '_';
    }

    static bool isWordPart(char character) {
        const unsigned char value = static_cast<unsigned char>(character);
        return std::isalnum(value) || character == '_';
    }

    void advance() {
        if (mSource[mOffset] == '\n') {
            ++mLine;
            mColumn = 1;
        } else {
            ++mColumn;
        }
        ++mOffset;
    }

    void fail(const Token& token, const std::string& message) {
        if (!mResult.error.empty()) return;
        mResult.error = message;
        mResult.line = token.line;
        mResult.column = token.column;
    }

    bool tokenize() {
        while (mOffset < mSource.size()) {
            const char character = mSource[mOffset];
            if (std::isspace(static_cast<unsigned char>(character))) {
                advance();
                continue;
            }
            if (character == '/' && mOffset + 1 < mSource.size() && mSource[mOffset + 1] == '/') {
                while (mOffset < mSource.size() && mSource[mOffset] != '\n') advance();
                continue;
            }
            if (character == '/' && mOffset + 1 < mSource.size() && mSource[mOffset + 1] == '*') {
                const Token start{TokenKind::Symbol, "/*", mLine, mColumn};
                advance();
                advance();
                bool closed = false;
                while (mOffset < mSource.size()) {
                    if (mSource[mOffset] == '*' && mOffset + 1 < mSource.size() &&
                        mSource[mOffset + 1] == '/') {
                        advance();
                        advance();
                        closed = true;
                        break;
                    }
                    advance();
                }
                if (!closed) {
                    fail(start, "unterminated block comment");
                    return false;
                }
                continue;
            }

            Token token;
            token.line = mLine;
            token.column = mColumn;
            if (character == '"') {
                token.kind = TokenKind::String;
                advance();
                bool closed = false;
                while (mOffset < mSource.size()) {
                    if (mSource[mOffset] == '"') {
                        advance();
                        closed = true;
                        break;
                    }
                    if (mSource[mOffset] == '\\' && mOffset + 1 < mSource.size()) advance();
                    if (mSource[mOffset] == '\n') break;
                    token.text.push_back(mSource[mOffset]);
                    advance();
                }
                if (!closed) {
                    fail(token, "unterminated string literal");
                    return false;
                }
            } else if (isWordStart(character)) {
                token.kind = TokenKind::Word;
                while (mOffset < mSource.size() && isWordPart(mSource[mOffset])) {
                    token.text.push_back(mSource[mOffset]);
                    advance();
                }
            } else {
                token.kind = TokenKind::Symbol;
                token.text.push_back(character);
                advance();
            }
            mTokens.push_back(std::move(token));
        }
        mTokens.push_back(Token{TokenKind::End, {}, mLine, mColumn});
        return true;
    }

    const Token& current() const {
        return mTokens[mPosition];
    }

    bool atEnd() const {
        return current().kind == TokenKind::End;
    }

    bool consume(const char* value) {
        if (current().text != value) return false;
        ++mPosition;
        return true;
    }

    bool expect(const char* value) {
        if (consume(value)) return true;
        fail(current(), std::string("expected '") + value + "'");
        return false;
    }

    bool readName(std::string& name) {
        if (current().kind != TokenKind::Word) {
            fail(current(), "expected identifier");
            return false;
        }
        name = current().text;
        ++mPosition;
        return true;
    }

    void readDirective() {
        const std::size_t directiveLine = current().line;
        ++mPosition;
        const bool include = current().text == "include";
        if (current().line == directiveLine && !atEnd()) ++mPosition;
        if (include && current().line == directiveLine) {
            if (current().kind != TokenKind::String) {
                fail(current(), "expected quoted include path");
                return;
            }
            mResult.effect.includes.push_back(current().text);
        }
        while (!atEnd() && current().line == directiveLine) ++mPosition;
    }

    void skipDeclaration() {
        int braces = 0;
        int parentheses = 0;
        while (!atEnd()) {
            if (current().text == "{") {
                ++braces;
            } else if (current().text == "}") {
                if (!braces) {
                    fail(current(), "unexpected closing brace");
                    return;
                }
                --braces;
                if (!braces && !parentheses) {
                    ++mPosition;
                    consume(";");
                    return;
                }
            } else if (current().text == "(") {
                ++parentheses;
            } else if (current().text == ")") {
                if (!parentheses) {
                    fail(current(), "unexpected closing parenthesis");
                    return;
                }
                --parentheses;
            } else if (current().text == ";" && !braces && !parentheses) {
                ++mPosition;
                return;
            }
            ++mPosition;
        }
        fail(current(), "unterminated declaration");
    }

    bool readShaderCall(std::string& name) {
        if (!expect("=")) return false;
        consume("compile");
        if (!readName(name) || !expect("(")) return false;
        int depth = 1;
        while (!atEnd() && depth) {
            if (current().text == "(") ++depth;
            if (current().text == ")") --depth;
            ++mPosition;
        }
        if (depth) {
            fail(current(), "unterminated shader call");
            return false;
        }
        return expect(";");
    }

    bool readPass(MaiGraphicsEffectPass& pass) {
        pass.line = current().line;
        ++mPosition;  // pass
        if (current().kind == TokenKind::Word && !readName(pass.name)) return false;
        if (!expect("{")) return false;
        while (!atEnd() && current().text != "}" && mResult.error.empty()) {
            const std::string command = current().text;
            ++mPosition;
            if (command == "vertex_shader" || command == "vertex_program") {
                if (!pass.vertexShader.empty()) {
                    fail(mTokens[mPosition - 1], "duplicate vertex shader in pass");
                    return false;
                }
                if (!readShaderCall(pass.vertexShader)) return false;
            } else if (command == "pixel_shader" || command == "pixel_program") {
                if (!pass.pixelShader.empty()) {
                    fail(mTokens[mPosition - 1], "duplicate pixel shader in pass");
                    return false;
                }
                if (!readShaderCall(pass.pixelShader)) return false;
            } else {
                fail(mTokens[mPosition - 1], "unsupported pass command");
                return false;
            }
        }
        if (!expect("}")) return false;
        if (pass.vertexShader.empty() || pass.pixelShader.empty()) {
            fail(mTokens[mPosition - 1], "pass requires vertex and pixel shaders");
            return false;
        }
        return true;
    }

    void readTechnique() {
        MaiGraphicsEffectTechnique technique;
        technique.line = current().line;
        ++mPosition;
        if (!readName(technique.name) || !expect("{")) return;
        for (const auto& previous : mResult.effect.techniques) {
            if (previous.name == technique.name) {
                fail(mTokens[mPosition - 2], "duplicate technique name");
                return;
            }
        }
        while (!atEnd() && current().text != "}" && mResult.error.empty()) {
            if (current().text != "pass") {
                fail(current(), "expected pass in technique");
                return;
            }
            MaiGraphicsEffectPass pass;
            if (!readPass(pass)) return;
            technique.passes.push_back(std::move(pass));
        }
        if (!expect("}")) return;
        consume(";");
        if (technique.passes.empty()) {
            fail(mTokens[mPosition - 1], "technique requires at least one pass");
            return;
        }
        mResult.effect.techniques.push_back(std::move(technique));
    }

    const std::string& mSource;
    std::vector<Token> mTokens;
    MaiGraphicsEffectParseResult mResult;
    std::size_t mOffset = 0;
    std::size_t mLine = 1;
    std::size_t mColumn = 1;
    std::size_t mPosition = 0;
};

}  // namespace

bool MaiGraphicsEffectParseResult::isOk() const {
    return error.empty();
}

MaiGraphicsEffectParseResult MaiGraphicsEffectParser::parse(const std::string& source) const {
    return EffectReader(source).read();
}
