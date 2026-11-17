#ifndef JSON_H
#define JSON_H

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdint.h>

#include "ds.h"

enum JsonError {
    JSON_OK = 0,
    JSON_OOM,
    JSON_ARENA_FULL,
    JSONE_GEN_NUM_INVALID,
    JSONE_GEN_DEPTH_LIMIT_EXCEEDED,
    JSONE_GEN_UTF8_INVALID,
    JSONE_PARSE_CHARACTER_UNEXPECTED,
    JSONE_PARSE_NUMBER_INVALID,
    JSONE_PARSE_STRING_UNCLOSED,
    JSONE_PARSE_BACKLASH_ESCAPE_INVALID,
    JSONE_PARSE_CONTROL_CHARACTER_UNESCAPED,
    JSONE_PARSE_HEX_ESCAPE_INVALID,
    JSONE_PARSE_UTF8_INVALID,
    JSONE_PARSE_TOKEN_UNEXPECTED,
    JSONE_PARSE_ARRAY_SEPARATOR_MISSING,
    JSONE_PARSE_OBJECT_SEPARATOR_MISSING,
    JSONE_PARSE_OBJECT_NAME_MISSING,
    JSONE_PARSE_NAME_SEPARATOR_MISSING,
    JSONE_PARSE_DEPTH_LIMIT_EXCEEDED,
};

enum JsonType { JSON_OBJECT, JSON_ARRAY, JSON_NUMBER, JSON_STRING, JSON_BOOLEAN, JSON_NULL };

typedef struct {
    Arena structure;
    Arena strings;
} JsonArena;

typedef struct JsonValue JsonValue;
typedef struct JsonMember JsonMember;

DEFINE_DA_TYPE(JsonObject, JsonMember, json_object_append);
DEFINE_DA_TYPE(JsonArray, JsonValue, json_array_append);

struct JsonValue {
    enum JsonType type;
    union {
        JsonObject object;
        JsonArray array;
        StringView string;
        double number;
        bool boolean;
    };
};

struct JsonMember {
    StringView name;
    JsonValue value;
};

enum JsonKind {
    JSON_KIND_BEGIN_ARRAY,
    JSON_KIND_BEGIN_OBJECT,
    JSON_KIND_END_ARRAY,
    JSON_KIND_END_OBJECT,
    JSON_KIND_NAME_SEPARATOR,
    JSON_KIND_VALUE_SEPARATOR,
    JSON_KIND_STRING,
    JSON_KIND_NUMBER,
    JSON_KIND_BOOLEAN,
    JSON_KIND_NULL,
    JSON_KIND_EOF,
};

typedef struct Token {
    enum JsonKind kind;
    union {
        StringView string;
        double number;
        bool boolean;
    };
} Token;

#define JSON_DEPTH_LIMIT 32

#define JSON_STR(...) #__VA_ARGS__

char *error_get_message(enum JsonError err);

// ==================== Creating JSON structures ====================

void json_arena_destroy(JsonArena *arena);
enum JsonError json_arena_create(JsonArena *arena, size_t structure_size, size_t strings_size);
JsonValue json_object(JsonObject object);
JsonValue json_array(JsonArray array);
JsonValue json_string(StringView string);
JsonValue json_number(double number);
JsonValue json_bool(bool boolean);
JsonValue json_null();
enum JsonError json_obj_add_with_sv(JsonArena *arena, JsonObject *object, StringView name_sv, JsonValue value);
enum JsonError json_obj_add(JsonArena *arena, JsonObject *object, const char *name, JsonValue value);
enum JsonError json_obj_add_string(JsonArena *arena, JsonObject *object, const char *name, const char *value);

// ==================== Accessing JSON structures ====================

const JsonValue *json_get(const JsonObject *object, StringView name);
const JsonObject *json_get_object(const JsonObject *object, StringView name);
const JsonArray *json_get_array(const JsonObject *object, StringView name);
const StringView *json_get_string(const JsonObject *object, StringView name);
const double *json_get_number(const JsonObject *object, StringView name);
const bool *json_get_bool(const JsonObject *object, StringView name);
const JsonValue *json_get_s(const JsonObject *object, const char *name);
const JsonObject *json_get_object_s(const JsonObject *object, const char *name);
const JsonArray *json_get_array_s(const JsonObject *object, const char *name);
const StringView *json_get_string_s(const JsonObject *object, const char *name);
const double *json_get_number_s(const JsonObject *object, const char *name);
const bool *json_get_bool_s(const JsonObject *object, const char *name);

// ==================== Parsing and generating JSON ====================

enum JsonError json_parse(JsonArena *arena, JsonValue **result, StringView *error_message, StringView json_sv);
enum JsonError json_generate(StringBuilder *sb, const JsonValue *value);
enum JsonError json_generate_pretty(StringBuilder *sb, const JsonValue *value, int pretty_width);

#ifdef JSON_IMPLEMENTATION

char *error_get_message(const enum JsonError err) {
    switch (err) {
        case JSON_OK:
            return "OK";
        case JSON_OOM:
            return "malloc failed";
        case JSON_ARENA_FULL:
            return "not enough space in arena";
        case JSONE_GEN_NUM_INVALID:
            return "infinity or NaN cannot be serialized";
        case JSONE_GEN_DEPTH_LIMIT_EXCEEDED:
            return "depth limit exceeded";
        case JSONE_GEN_UTF8_INVALID:
            return "invalid UTF8";
        case JSONE_PARSE_CHARACTER_UNEXPECTED:
            return "unexpected character";
        case JSONE_PARSE_NUMBER_INVALID:
            return "invalid number";
        case JSONE_PARSE_STRING_UNCLOSED:
            return "string is not closed";
        case JSONE_PARSE_BACKLASH_ESCAPE_INVALID:
            return "invalid backslash escape";
        case JSONE_PARSE_CONTROL_CHARACTER_UNESCAPED:
            return "control characters must be escaped";
        case JSONE_PARSE_HEX_ESCAPE_INVALID:
            return "invalid hex escape";
        case JSONE_PARSE_UTF8_INVALID:
            return "invalid utf8";
        case JSONE_PARSE_TOKEN_UNEXPECTED:
            return "unexpected token";
        case JSONE_PARSE_ARRAY_SEPARATOR_MISSING:
            return "array elements must be separated by a ','";
        case JSONE_PARSE_OBJECT_SEPARATOR_MISSING:
            return "object members must be separated by a ','";
        case JSONE_PARSE_OBJECT_NAME_MISSING:
            return "object members must start with a name";
        case JSONE_PARSE_NAME_SEPARATOR_MISSING:
            return "object member names must be followed by a ':'";
        case JSONE_PARSE_DEPTH_LIMIT_EXCEEDED:
            return "depth limit exceeded";
        default:
            return "Error code not recognized";
    }
}

// ==================== General macros ====================

#define JSON_TRY(expr)                         \
    do {                                       \
        enum JsonError try_err = (expr);       \
        if (try_err != JSON_OK) { return try_err; } \
    } while (0)

#define JSON_DEFER(code) \
    do {                 \
        err = code;      \
        goto defer;      \
    } while (0)

#define JSON_TRY_OR_DEFER(expr)        \
    do {                               \
        err = (expr);                  \
        if (err != JSON_OK) { goto defer; } \
    } while (0)

// ==================== Creating JSON structures ====================

void json_arena_destroy(JsonArena *arena) {
    arena_destroy(&arena->structure);
    arena_destroy(&arena->strings);
}

enum JsonError json_arena_create(JsonArena *arena, const size_t structure_size, const size_t strings_size) {
    *arena = (JsonArena){0};
    arena->structure = arena_create(structure_size, 8);
    if (!arena->structure.data) return JSON_OOM;

    arena->strings = arena_create(strings_size, 1);
    if (!arena->strings.data) {
        json_arena_destroy(arena);
        return JSON_OOM;
    }
    return JSON_OK;
}

JsonValue json_object(const JsonObject object) { return (JsonValue){JSON_OBJECT, .object = object}; }
JsonValue json_array(const JsonArray array) { return (JsonValue){JSON_ARRAY, .array = array}; }
JsonValue json_string(const StringView string) { return (JsonValue){JSON_STRING, .string = string}; }
JsonValue json_number(const double number) { return (JsonValue){JSON_NUMBER, .number = number}; }
JsonValue json_bool(const bool boolean) { return (JsonValue){JSON_BOOLEAN, .boolean = boolean}; }
JsonValue json_null() { return (JsonValue){JSON_NULL}; }

IMPLEMENT_DA_APPEND(json_object_append, JsonObject, JsonMember)
IMPLEMENT_DA_APPEND(json_array_append, JsonArray, JsonValue)

enum JsonError json_obj_add_with_sv(JsonArena *arena, JsonObject *object, const StringView name_sv,
                                    const JsonValue value) {
    // We take an SV for the name, but we want to store the entire JSON together in the arena with ownership of it.
    const StringView name = sv_copy_to_arena(&arena->strings, name_sv);
    if (!name.chars) return JSON_ARENA_FULL;

    return json_object_append(&arena->structure, object, (JsonMember){name, value}) ? JSON_OK : JSON_ARENA_FULL;
}

enum JsonError json_obj_add(JsonArena *arena, JsonObject *object, const char *name, const JsonValue value) {
    return json_obj_add_with_sv(arena, object, sv_on_cstr(name), value);
}

enum JsonError json_obj_add_string(JsonArena *arena, JsonObject *object, const char *name, const char *value) {
    const StringView str = cstr_copy_to_arena(&arena->strings, value);
    return json_obj_add_with_sv(arena, object, sv_on_cstr(name), json_string(str));
}

// ==================== Lexing JSON ====================

static enum JsonError json_lex_next_number(Token *token, StringView *sv) {
    enum State {
        START,
        AFTER_MINUS,
        LEADING_ZERO,
        LEADING_DIGIT,
        INT_DIGITS,
        FRACTION_DIGITS,
        EXPONENT,
        EXPONENT_SIGN,
        EXPONENT_DIGITS,
        END
    };

    const char *s = sv->chars;
    const size_t len = sv->length;

    int i = 0;
    enum State state = START;
    while (1) {
        switch (state) {
            case START:
                if (len == i) return JSONE_PARSE_CHARACTER_UNEXPECTED;
                if (isdigit(s[i])) {
                    if (s[i] != '0') {
                        state = LEADING_DIGIT;
                    } else {
                        state = LEADING_ZERO;
                    }
                } else if (s[i] == '-') {
                    state = AFTER_MINUS;
                } else {
                    return JSONE_PARSE_CHARACTER_UNEXPECTED;
                }
                break;
            case AFTER_MINUS:
                if (len == i) return JSONE_PARSE_CHARACTER_UNEXPECTED;
                if (isdigit(s[i])) {
                    if (s[i] != '0') {
                        state = LEADING_DIGIT;
                    } else {
                        state = LEADING_ZERO;
                    }
                } else {
                    return JSONE_PARSE_CHARACTER_UNEXPECTED;
                }
                break;
            case LEADING_ZERO:
                if (len == i) {  // NOLINT(bugprone-branch-clone)
                    state = END;
                } else if (s[i] == '.') {
                    state = FRACTION_DIGITS;
                } else if (s[i] == 'e' || s[i] == 'E') {
                    state = EXPONENT;
                } else {
                    state = END;
                }
                break;
            case LEADING_DIGIT:
            case INT_DIGITS:
                if (len == i) {  // NOLINT(bugprone-branch-clone)
                    state = END;
                } else if (isdigit(s[i])) {
                    state = INT_DIGITS;
                } else if (s[i] == '.') {
                    state = FRACTION_DIGITS;
                } else if (s[i] == 'e' || s[i] == 'E') {
                    state = EXPONENT;
                } else {
                    state = END;
                }
                break;
            case FRACTION_DIGITS:
                // If we go to the END state here right after the . like "1." it is not valid according to RFC. JS
                // accepts "1." though.
                if (len == i) {  // NOLINT(bugprone-branch-clone)
                    state = END;
                } else if (isdigit(s[i])) {
                    state = FRACTION_DIGITS;
                } else if (s[i] == 'e' || s[i] == 'E') {
                    state = EXPONENT;
                } else {
                    state = END;
                }
                break;
            case EXPONENT:
                if (len == i) return JSONE_PARSE_CHARACTER_UNEXPECTED;
                if (isdigit(s[i])) {
                    state = EXPONENT_DIGITS;
                } else if (s[i] == '+' || s[i] == '-') {
                    state = EXPONENT_SIGN;
                } else {
                    return JSONE_PARSE_CHARACTER_UNEXPECTED;
                }
                break;
            case EXPONENT_SIGN:
                if (len == i) return JSONE_PARSE_CHARACTER_UNEXPECTED;
                if (isdigit(s[i])) {
                    state = EXPONENT_DIGITS;
                } else {
                    return JSONE_PARSE_CHARACTER_UNEXPECTED;
                }
                break;
            case EXPONENT_DIGITS:
                if (len == i || !isdigit(s[i])) {
                    state = END;
                } else {
                    state = EXPONENT_DIGITS;
                }
                break;
            case END: {
                i--;
                // Copy to buffer to use strtod which expects null-termination
                if (i >= 60) return JSONE_PARSE_NUMBER_INVALID;
                char buf[64] = {0};
                memcpy(buf, sv->chars, i);

                char *end;
                *token = (Token){JSON_KIND_NUMBER, .number = strtod(buf, &end)};

                // If the state machine is correct strtod should parse the same length
                assert(end - buf == i);

                sv_chop_left(sv, i);
                return JSON_OK;
            }
        }
        i++;
    }
}

// We allow / when parsing but no need to generate it
static const char PARSE_BACKSLASH_ESCAPE_MAP[256] = {
    ['"'] = '"', ['\\'] = '\\', ['b'] = '\b', ['f'] = '\f', ['n'] = '\n', ['r'] = '\r', ['t'] = '\t', ['/'] = '/'};
static const char GENERATE_BACKSLASH_ESCAPE_MAP[256] = {
    ['"'] = '"', ['\\'] = '\\', ['\b'] = 'b', ['\f'] = 'f', ['\n'] = 'n', ['\r'] = 'r', ['\t'] = 't'};

// region UTF8 decode state machine table
// clang-format off
static const uint8_t UTF8D[] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
    8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
    0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
    0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
    0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
    1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
    1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
    1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
};
// clang-format on
// endregion

// https://bjoern.hoehrmann.de/utf-8/decoder/dfa/ without calculating the code point
static uint32_t utf8_validate(uint32_t *state, const uint32_t byte) {
    const uint32_t type = UTF8D[byte];

    *state = UTF8D[256 + *state * 16 + type];
    return *state;
}

static enum JsonError convert_four_hex_digits(uint16_t *dest, const StringView sv) {
    if (sv.length < 4) return JSONE_PARSE_HEX_ESCAPE_INVALID;

    uint16_t sum = 0;
    for (int i = 0; i < 4; ++i) {
        sum *= 16;

        const char c = sv.chars[i];
        if ('0' <= c && c <= '9') {
            sum += c - '0';
        } else if ('A' <= c && c <= 'F') {
            sum += c - 'A' + 10;
        } else if ('a' <= c && c <= 'f') {
            sum += c - 'a' + 10;
        } else {
            return JSONE_PARSE_HEX_ESCAPE_INVALID;
        }
    }
    *dest = sum;
    return JSON_OK;
}

static enum JsonError json_lex_next_string(Arena *string_arena, Token *token, StringView *sv) {
    if (sv->length < 0 || *sv->chars != '"') return JSONE_PARSE_CHARACTER_UNEXPECTED;
    sv_chop_left(sv, 1);

    enum JsonError err;
    StringBuilder sb = sb_create_and_borrow_arena(string_arena);

    while (sv->length > 0) {
        if (*sv->chars == '"') {
            sv_chop_left(sv, 1);

            *token = (Token){JSON_KIND_STRING, .string = sb_complete_and_release_arena(&sb)};
            return JSON_OK;
        }

        if (*sv->chars == '\\') {
            // Backslash escape cannot be cut off by EOF
            if (sv->length < 2) JSON_DEFER(JSONE_PARSE_BACKLASH_ESCAPE_INVALID);

            if (sv->chars[1] == 'u') {
                if (sv->length < 6) JSON_DEFER(JSONE_PARSE_HEX_ESCAPE_INVALID);

                uint16_t n;
                JSON_TRY_OR_DEFER(convert_four_hex_digits(&n, sv_slice(*sv, 2, 6)));

                // https://www.rfc-editor.org/info/rfc8259/#section-8.2 notes that the spec allows 'bit sequences that
                // cannot encode Unicode characters; for example, "\uDEAD" (a single unpaired UTF-16 surrogate)'
                // I've decided to reject invalid characters instead so they don't cause problems elsewhere.

                // UTF-16 low surrogate without a preceding high surrogate
                if (0xDC00 <= n && n <= 0xDFFF) JSON_DEFER(JSONE_PARSE_HEX_ESCAPE_INVALID);

                uint32_t code_point;
                int consumed_characters = 6;
                if (0xD800 <= n && n <= 0xDBFF) {  // UTF-16 high surrogate, MUST be followed by low surrogate
                    consumed_characters += 6;
                    if (sv->length < 12 || sv->chars[6] != '\\' || sv->chars[7] != 'u')
                        JSON_DEFER(JSONE_PARSE_HEX_ESCAPE_INVALID);

                    uint16_t low_surrogate;
                    JSON_TRY_OR_DEFER(convert_four_hex_digits(&low_surrogate, sv_slice(*sv, 8, 12)));

                    if (low_surrogate < 0xDC00 || 0xDFFF < low_surrogate) JSON_DEFER(JSONE_PARSE_HEX_ESCAPE_INVALID);

                    code_point = 0x10000 + (((n - 0xD800) << 10) | (low_surrogate - 0xDC00));
                } else {  // Not a surrogate so must be a single code point
                    code_point = n;
                }

                if (code_point <= 0x007f) {
                    if (!sb_append_char(&sb, code_point)) JSON_DEFER(JSON_ARENA_FULL);
                } else if (code_point <= 0x07FF) {
                    if (!sb_append_char(&sb, 0b11000000 | ((code_point >> 6) & 0b00011111))) JSON_DEFER(JSON_ARENA_FULL);
                    if (!sb_append_char(&sb, 0b10000000 | (code_point & 0b00111111))) JSON_DEFER(JSON_ARENA_FULL);
                } else if (code_point <= 0xFFFF) {
                    if (!sb_append_char(&sb, 0b11100000 | ((code_point >> 12) & 0b00001111))) JSON_DEFER(JSON_ARENA_FULL);
                    if (!sb_append_char(&sb, 0b10000000 | ((code_point >> 6) & 0b00111111))) JSON_DEFER(JSON_ARENA_FULL);
                    if (!sb_append_char(&sb, 0b10000000 | (code_point & 0b00111111))) JSON_DEFER(JSON_ARENA_FULL);
                } else if (code_point <= 0x10FFFF) {
                    if (!sb_append_char(&sb, 0b11110000 | ((code_point >> 18) & 0b00000111))) JSON_DEFER(JSON_ARENA_FULL);
                    if (!sb_append_char(&sb, 0b10000000 | ((code_point >> 12) & 0b00111111))) JSON_DEFER(JSON_ARENA_FULL);
                    if (!sb_append_char(&sb, 0b10000000 | ((code_point >> 6) & 0b00111111))) JSON_DEFER(JSON_ARENA_FULL);
                    if (!sb_append_char(&sb, 0b10000000 | (code_point & 0b00111111))) JSON_DEFER(JSON_ARENA_FULL);
                } else {
                    assert(0);
                }

                sv_chop_left(sv, consumed_characters);
            } else {
                // Valid escapes are "\bfnrt/
                const unsigned char c = PARSE_BACKSLASH_ESCAPE_MAP[sv->chars[1]];
                if (c == 0) JSON_DEFER(JSONE_PARSE_BACKLASH_ESCAPE_INVALID);

                if (!sb_append_char(&sb, c)) JSON_DEFER(JSON_ARENA_FULL);
                sv_chop_left(sv, 2);
            }
        } else if ((uint8_t)*sv->chars < 128) {
            // Control characters must be escaped and cannot be directly in strings
            if (*sv->chars < 32) JSON_DEFER(JSONE_PARSE_CONTROL_CHARACTER_UNESCAPED);

            if (!sb_append_char(&sb, *sv->chars)) JSON_DEFER(JSON_ARENA_FULL);
            sv_chop_left(sv, 1);
        } else {
            const uint8_t *s = (uint8_t *)sv->chars;
            int length = 0;
            uint32_t state = 0;
            while (length++ < sv->length) {
                if (!utf8_validate(&state, *s++)) break;
            }

            if (state == 0) {  // OK
                if (!sb_append_sv(&sb, (StringView){length, sv->chars})) JSON_DEFER(JSON_ARENA_FULL);
                sv_chop_left(sv, length);
            } else {
                JSON_DEFER(JSONE_PARSE_UTF8_INVALID);
            }
        }
    }
    err = JSONE_PARSE_STRING_UNCLOSED;
defer:
    sb_complete_and_release_arena(&sb);
    return err;
}

static enum JsonError json_lex_next_token(Arena *string_arena, Token *token, StringView *sv) {
    while (sv->length > 0) {
        const uint8_t c = *sv->chars;
        switch (c) {
            case '[':
                *token = (Token){JSON_KIND_BEGIN_ARRAY};
                sv_chop_left(sv, 1);
                return JSON_OK;
            case '{':
                *token = (Token){JSON_KIND_BEGIN_OBJECT};
                sv_chop_left(sv, 1);
                return JSON_OK;
            case ']':
                *token = (Token){JSON_KIND_END_ARRAY};
                sv_chop_left(sv, 1);
                return JSON_OK;
            case '}':
                *token = (Token){JSON_KIND_END_OBJECT};
                sv_chop_left(sv, 1);
                return JSON_OK;
            case ':':
                *token = (Token){JSON_KIND_NAME_SEPARATOR};
                sv_chop_left(sv, 1);
                return JSON_OK;
            case ',':
                *token = (Token){JSON_KIND_VALUE_SEPARATOR};
                sv_chop_left(sv, 1);
                return JSON_OK;
            case '"':
                return json_lex_next_string(string_arena, token, sv);
            case 't':
                if (sv->length >= 4 && memcmp(sv->chars, "true", 4) == 0) {
                    *token = (Token){JSON_KIND_BOOLEAN, .boolean = true};
                    sv_chop_left(sv, 4);
                    return JSON_OK;
                }
                return JSONE_PARSE_CHARACTER_UNEXPECTED;
            case 'f':
                if (sv->length >= 5 && memcmp(sv->chars, "false", 5) == 0) {
                    *token = (Token){JSON_KIND_BOOLEAN, .boolean = false};
                    sv_chop_left(sv, 5);
                    return JSON_OK;
                }
                return JSONE_PARSE_CHARACTER_UNEXPECTED;
            case 'n':
                if (sv->length >= 4 && memcmp(sv->chars, "null", 4) == 0) {
                    *token = (Token){JSON_KIND_NULL};
                    sv_chop_left(sv, 4);
                    return JSON_OK;
                }
                return JSONE_PARSE_CHARACTER_UNEXPECTED;
            default:
                if (isspace(c)) {
                    sv_chop_left(sv, 1);
                } else if (isdigit(c) || c == '-') {
                    return json_lex_next_number(token, sv);
                } else {
                    return JSONE_PARSE_CHARACTER_UNEXPECTED;
                }
        }
    }

    *token = (Token){JSON_KIND_EOF};
    return JSON_OK;
}

// ==================== Parsing JSON ====================

typedef struct JsonParserState {
    JsonArena *arena;
    StringView *json_sv;
    const size_t json_length;
} JsonParserState;

static enum JsonError json_parse_value(const JsonParserState parser, JsonValue *result, const Token *value_token,
                                       const int depth) {
    if (depth > JSON_DEPTH_LIMIT) return JSONE_PARSE_DEPTH_LIMIT_EXCEEDED;
    switch (value_token->kind) {
        case JSON_KIND_BEGIN_ARRAY: {
            JsonArray array = {0};
            bool first = true;
            while (1) {
                Token element_token;
                JSON_TRY(json_lex_next_token(&parser.arena->strings, &element_token, parser.json_sv));

                if (element_token.kind == JSON_KIND_END_ARRAY) {
                    // Empty array
                    if (first) break;
                    return JSONE_PARSE_TOKEN_UNEXPECTED;
                }
                first = false;

                JsonValue element_value;
                JSON_TRY(json_parse_value(parser, &element_value, &element_token, depth + 1));
                if (!json_array_append(&parser.arena->structure, &array, element_value)) return JSON_ARENA_FULL;

                Token seperator_token;
                JSON_TRY(json_lex_next_token(&parser.arena->strings, &seperator_token, parser.json_sv));

                if (seperator_token.kind == JSON_KIND_END_ARRAY) break;
                if (seperator_token.kind != JSON_KIND_VALUE_SEPARATOR) { return JSONE_PARSE_ARRAY_SEPARATOR_MISSING; }
            }
            *result = json_array(array);
            break;
        }
        case JSON_KIND_BEGIN_OBJECT: {
            JsonObject object = {0};
            bool first = true;
            while (1) {
                Token name_token;
                JSON_TRY(json_lex_next_token(&parser.arena->strings, &name_token, parser.json_sv));

                if (name_token.kind == JSON_KIND_END_OBJECT) {
                    // Empty object
                    if (first) break;
                    return JSONE_PARSE_TOKEN_UNEXPECTED;
                }
                first = false;

                if (name_token.kind != JSON_KIND_STRING) { return JSONE_PARSE_OBJECT_NAME_MISSING; }

                Token name_seperator_token;
                JSON_TRY(json_lex_next_token(&parser.arena->strings, &name_seperator_token, parser.json_sv));
                if (name_seperator_token.kind != JSON_KIND_NAME_SEPARATOR) {
                    return JSONE_PARSE_NAME_SEPARATOR_MISSING;
                }

                Token member_token;
                JSON_TRY(json_lex_next_token(&parser.arena->strings, &member_token, parser.json_sv));

                JsonValue member_value;
                JSON_TRY(json_parse_value(parser, &member_value, &member_token, depth + 1));
                JSON_TRY(json_obj_add_with_sv(parser.arena, &object, name_token.string, member_value));

                Token value_seperator_token;
                JSON_TRY(json_lex_next_token(&parser.arena->strings, &value_seperator_token, parser.json_sv));

                if (value_seperator_token.kind == JSON_KIND_END_OBJECT) break;
                if (value_seperator_token.kind != JSON_KIND_VALUE_SEPARATOR) {
                    return JSONE_PARSE_OBJECT_SEPARATOR_MISSING;
                }
            }
            *result = json_object(object);
            break;
        }
        case JSON_KIND_STRING:
            *result = json_string(value_token->string);
            break;
        case JSON_KIND_NUMBER:
            *result = json_number(value_token->number);
            break;
        case JSON_KIND_BOOLEAN:
            *result = json_bool(value_token->boolean);
            break;
        case JSON_KIND_NULL:
            *result = json_null();
            break;
        default:
            return JSONE_PARSE_TOKEN_UNEXPECTED;
    }
    return JSON_OK;
}

static enum JsonError json_parser_handle_error(const JsonParserState parser, StringView *error_message,
                                               const enum JsonError err) {
    if (error_message) {
        *error_message = sv_create_formatted(&parser.arena->strings, "Error parsing JSON at byte %zu: %s",
                                             parser.json_length - parser.json_sv->length, error_get_message(err));
    }
    return err;
}

enum JsonError json_parse(JsonArena *arena, JsonValue **result, StringView *error_message, StringView json_sv) {
    const JsonParserState parser = {arena, &json_sv, json_sv.length};

    Token token;
    {
        const enum JsonError err = json_lex_next_token(&parser.arena->strings, &token, parser.json_sv);
        if (err != JSON_OK) return json_parser_handle_error(parser, error_message, err);
    }

    JsonValue *v = arena_alloc(&arena->structure, sizeof(JsonValue));
    if (!v) return JSON_ARENA_FULL;

    {
        const enum JsonError err = json_parse_value(parser, v, &token, 0);
        if (err != JSON_OK) return json_parser_handle_error(parser, error_message, err);
    }

    *result = v;
    return JSON_OK;
}

// ==================== Reading JSON structures ====================

const JsonValue *json_get(const JsonObject *object, const StringView name) {
    for (size_t i = 0; i < object->count; ++i) {
        if (sv_equals(name, object->items[i].name)) return &object->items[i].value;
    }
    return NULL;
}

const JsonObject *json_get_object(const JsonObject *object, const StringView name) {
    const JsonValue *m = json_get(object, name);
    if (!m) return NULL;
    if (m->type == JSON_OBJECT) return &m->object;
    return NULL;
}
const JsonArray *json_get_array(const JsonObject *object, const StringView name) {
    const JsonValue *m = json_get(object, name);
    if (!m) return NULL;
    if (m->type == JSON_ARRAY) return &m->array;
    return NULL;
}
const StringView *json_get_string(const JsonObject *object, const StringView name) {
    const JsonValue *m = json_get(object, name);
    if (!m) return NULL;
    if (m->type == JSON_STRING) return &m->string;
    return NULL;
}
const double *json_get_number(const JsonObject *object, const StringView name) {
    const JsonValue *m = json_get(object, name);
    if (!m) return NULL;
    if (m->type == JSON_NUMBER) return &m->number;
    return NULL;
}
const bool *json_get_bool(const JsonObject *object, const StringView name) {
    const JsonValue *m = json_get(object, name);
    if (!m) return NULL;
    if (m->type == JSON_BOOLEAN) return &m->boolean;
    return NULL;
}

const JsonValue *json_get_s(const JsonObject *object, const char *name) { return json_get(object, sv_on_cstr(name)); }

const JsonObject *json_get_object_s(const JsonObject *object, const char *name) {
    return json_get_object(object, sv_on_cstr(name));
}
const JsonArray *json_get_array_s(const JsonObject *object, const char *name) {
    return json_get_array(object, sv_on_cstr(name));
}
const StringView *json_get_string_s(const JsonObject *object, const char *name) {
    return json_get_string(object, sv_on_cstr(name));
}
const double *json_get_number_s(const JsonObject *object, const char *name) {
    return json_get_number(object, sv_on_cstr(name));
}
const bool *json_get_bool_s(const JsonObject *object, const char *name) {
    return json_get_bool(object, sv_on_cstr(name));
}

// ==================== Generating JSON from a structure ====================

static enum JsonError json_generate_indentation(StringBuilder *sb, const int indent_width, const int depth) {
    if (!sb_append_char(sb, '\n')) return JSON_ARENA_FULL;
    for (int i = 0; i < indent_width * depth; ++i) {
        if (!sb_append_char(sb, ' ')) return JSON_ARENA_FULL;
    }
    return JSON_OK;
}

static enum JsonError json_generate_string(StringBuilder *sb, StringView sv) {
    if (!sb_append_char(sb, '"')) return JSON_ARENA_FULL;
    while (sv.length > 0) {
        if ((uint8_t)*sv.chars < 128) {
            const uint8_t escaped = GENERATE_BACKSLASH_ESCAPE_MAP[*sv.chars];
            if (escaped) {
                if (!sb_append_char(sb, '\\')) return JSON_ARENA_FULL;
                if (!sb_append_char(sb, escaped)) return JSON_ARENA_FULL;
            } else if (*sv.chars < 32) {
                if (!sb_append_formatted(sb, "\\u%.4X", *sv.chars)) return JSON_ARENA_FULL;
            } else {
                if (!sb_append_char(sb, *sv.chars)) return JSON_ARENA_FULL;
            }
            sv_chop_left(&sv, 1);
        } else {
            const uint8_t *s = (uint8_t *)sv.chars;
            int length = 0;
            uint32_t state = 0;
            while (length++ < sv.length) {
                if (!utf8_validate(&state, *s++)) break;
            }

            if (state == 0) {  // OK
                if (!sb_append_sv(sb, (StringView){length, sv.chars})) return JSON_ARENA_FULL;
                sv_chop_left(&sv, length);
            } else {
                return JSONE_GEN_UTF8_INVALID;
            }
        }
    }
    if (!sb_append_char(sb, '"')) return JSON_ARENA_FULL;
    return JSON_OK;
}

static enum JsonError json_generate_inner(StringBuilder *sb, const JsonValue *value, const int pretty,
                                          const int depth) {
    if (depth > JSON_DEPTH_LIMIT) return JSONE_GEN_DEPTH_LIMIT_EXCEEDED;
    switch (value->type) {
        case JSON_OBJECT:
            if (!sb_append_char(sb, '{')) return JSON_ARENA_FULL;
            for (size_t i = 0; i < value->object.count; ++i) {
                if (pretty > 0) JSON_TRY(json_generate_indentation(sb, pretty, depth + 1));

                const JsonMember member = value->object.items[i];

                JSON_TRY(json_generate_string(sb, member.name));
                if (!sb_append_char(sb, ':')) return JSON_ARENA_FULL;

                // When pretty-formatting add a space after the colon
                if (pretty > 0)
                    if (!sb_append_char(sb, ' ')) return JSON_ARENA_FULL;

                JSON_TRY(json_generate_inner(sb, &member.value, pretty, depth + 1));

                // Last in object has no comma
                if (i != value->object.count - 1)
                    if (!sb_append_char(sb, ',')) return JSON_ARENA_FULL;
            }
            if (pretty > 0 && value->object.count > 0) JSON_TRY(json_generate_indentation(sb, pretty, depth));

            if (!sb_append_char(sb, '}')) return JSON_ARENA_FULL;
            break;
        case JSON_ARRAY:
            if (!sb_append_char(sb, '[')) return JSON_ARENA_FULL;
            for (size_t i = 0; i < value->array.count; ++i) {
                if (pretty > 0) JSON_TRY(json_generate_indentation(sb, pretty, depth + 1));

                JSON_TRY(json_generate_inner(sb, value->array.items + i, pretty, depth + 1));

                // Last in array has no comma
                if (i != value->array.count - 1)
                    if (!sb_append_char(sb, ',')) return JSON_ARENA_FULL;
            }
            if (pretty > 0 && value->array.count > 0) JSON_TRY(json_generate_indentation(sb, pretty, depth));

            if (!sb_append_char(sb, ']')) return JSON_ARENA_FULL;
            break;
        case JSON_NUMBER:
            if (!isfinite(value->number)) return JSONE_GEN_NUM_INVALID;
            return sb_append_formatted(sb, "%.*G", DBL_DECIMAL_DIG, value->number) ? JSON_OK : JSON_ARENA_FULL;
        case JSON_STRING:
            return json_generate_string(sb, value->string);
        case JSON_BOOLEAN:
            return sb_append_sv(sb, sv_on_cstr(value->boolean ? "true" : "false")) ? JSON_OK : JSON_ARENA_FULL;
        case JSON_NULL:
            return sb_append_sv(sb, sv_on_cstr("null")) ? JSON_OK : JSON_ARENA_FULL;
    }
    return JSON_OK;
}

enum JsonError json_generate(StringBuilder *sb, const JsonValue *value) { return json_generate_inner(sb, value, 0, 0); }

enum JsonError json_generate_pretty(StringBuilder *sb, const JsonValue *value, const int pretty_width) {
    return json_generate_inner(sb, value, pretty_width, 0);
}

#endif  // JSON_IMPLEMENTATION
#endif  // JSON_H

// TODO null pointer handling?