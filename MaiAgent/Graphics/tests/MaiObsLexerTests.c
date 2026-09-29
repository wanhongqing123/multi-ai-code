#include <stdio.h>

#include <util/cf-lexer.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static void test_effect_tokens(void) {
    struct cf_lexer lexer;
    const char* expected[] = {"technique", "Draw", "{", "pass", "Main", "{", "}", "}"};
    size_t index = 0;
    cf_lexer_init(&lexer);
    CHECK(
        cf_lexer_lex(&lexer, "tech\\\nnique Draw { /* comment */ pass Main {} }", "memory.effect"));
    for (size_t token_index = 0; token_index < lexer.tokens.num; ++token_index) {
        const struct cf_token* token = &lexer.tokens.array[token_index];
        if (token->type == CFTOKEN_SPACETAB || token->type == CFTOKEN_NEWLINE ||
            token->type == CFTOKEN_NONE)
            continue;
        CHECK(index < sizeof(expected) / sizeof(expected[0]));
        if (index < sizeof(expected) / sizeof(expected[0]))
            CHECK(strref_cmp(&token->str, expected[index]) == 0);
        ++index;
    }
    CHECK(index == sizeof(expected) / sizeof(expected[0]));
    cf_lexer_free(&lexer);
}

static void test_unterminated_comment(void) {
    struct cf_lexer lexer;
    cf_lexer_init(&lexer);
    CHECK(!cf_lexer_lex(&lexer, "technique Draw { /* unfinished", "memory.effect"));
    cf_lexer_free(&lexer);
}

int main(void) {
    test_effect_tokens();
    test_unterminated_comment();
    return failures ? 1 : 0;
}
