#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Parsed OBS-style effect declarations. The parser is deliberately independent of file I/O,
// graphics devices, and shader compilers. Include paths are reported but not resolved; callers
// must supply the combined source or resolve includes before compiling a GPU effect. Parsing
// succeeds only for supported technique/pass syntax and does not promise shader compilation.
struct MaiGraphicsEffectPass {
    std::string name;
    std::string vertexShader;
    std::string pixelShader;
    std::size_t line = 0;
};

struct MaiGraphicsEffectTechnique {
    std::string name;
    std::vector<MaiGraphicsEffectPass> passes;
    std::size_t line = 0;
};

struct MaiGraphicsEffectDefinition {
    std::vector<std::string> includes;
    std::vector<MaiGraphicsEffectTechnique> techniques;
};

struct MaiGraphicsEffectParseResult {
    MaiGraphicsEffectDefinition effect;
    std::string error;
    std::size_t line = 0;
    std::size_t column = 0;

    bool isOk() const;
};

class MaiGraphicsEffectParser {
public:
    // Parses source in memory. Errors include a 1-based line/column and no partial effect is
    // returned. Line endings may be LF or CRLF. The parser accepts comments and nested shader
    // function bodies; preprocessor directives other than #include are skipped, not evaluated.
    MaiGraphicsEffectParseResult parse(const std::string& source) const;
};
