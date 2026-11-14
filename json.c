#include <assert.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ==================== General macros ====================

#define STR(...) #__VA_ARGS__
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) fprintf(stderr, "[WARN] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)

enum Error {
    OK = 0,
    MALLOC_FAIL,
    ARENA_FULL,
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

char *error_get_message(const enum Error err) {
    switch (err) {
        case OK:
            return "OK";
        case MALLOC_FAIL:
            return "malloc failed";
        case ARENA_FULL:
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

#define TRY(expr)                                                          \
    do {                                                                   \
        enum Error try_err = (expr);                                       \
        if (try_err != OK) {                                               \
            LOG_WARN("Error %d: %s", try_err, error_get_message(try_err)); \
            return try_err;                                                \
        }                                                                  \
    } while (0)

#define DEFER(code) \
    do {            \
        err = code; \
        goto defer; \
    } while (0)

// ==================== Allocators ====================

typedef struct {
    void *data;
    size_t alignment;
    size_t used;
    size_t capacity;
} Arena;

static size_t align_up(const size_t x, const size_t alignment) {
    assert(__builtin_popcountll(alignment) == 1);
    return x + (alignment - 1) & ~(alignment - 1);
}

Arena arena_create(const size_t capacity, const size_t alignment) {
    void *data = aligned_alloc(alignment, align_up(capacity, alignment));

    if (!data) return (Arena){0};

    return (Arena){data, alignment, 0, capacity};
}

void arena_destroy(Arena *arena) {
    free(arena->data);
    *arena = (Arena){0};
}

void arena_clear(Arena *arena) { arena->used = 0; }

void *arena_alloc(Arena *arena, const size_t size) {
    const size_t offset = align_up(arena->used, arena->alignment);

    if (offset > arena->capacity || size > arena->capacity - offset) { return NULL; }

    void *ptr = arena->data + offset;
    arena->used = offset + size;

    return ptr;
}

void *arena_realloc(Arena *arena, const void *old_ptr, const size_t old_size, const size_t new_size) {
    assert(new_size > old_size);

    const size_t offset = align_up(arena->used, arena->alignment);

    if (offset > arena->capacity || new_size > arena->capacity - offset) return NULL;

    void *new_ptr = arena->data + offset;
    arena->used = offset + new_size;

    memcpy(new_ptr, old_ptr, old_size);
    return new_ptr;
}

// ==================== Strings ====================

typedef struct StringView {
    size_t length;
    char *chars;
} StringView;

typedef struct String {
    size_t length;
    char *chars;
} String;

typedef struct StringBuilder {
    Arena *arena;
    size_t length;
    size_t capacity;
    char *chars;
} StringBuilder;

#define SV_FMT "%.*s"
#define SV_ARG(sv) (int)sv.length, sv.chars

StringView sv_on_string(const String string) { return (StringView){string.length, string.chars}; }

StringView sv_on_c_string(const char *c_string) { return (StringView){strlen(c_string), (char *)c_string}; }

enum Error copy_sv_to_string(Arena *string_arena, const StringView sv, String *string) {
    string->chars = arena_alloc(string_arena, sv.length);
    if (!string->chars) return ARENA_FULL;

    memcpy(string->chars, sv.chars, sv.length);
    string->length = sv.length;
    return OK;
}

enum Error copy_c_string_to_string(Arena *string_arena, const char *c_string, String *string) {
    return copy_sv_to_string(string_arena, sv_on_c_string(c_string), string);
}

enum Error sv_create_formatted(Arena *string_arena, StringView *sv, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
enum Error sv_create_formatted(Arena *string_arena, StringView *sv, const char *format, ...) {
    va_list args, args_copy;
    va_start(args, format);

    va_copy(args_copy, args);
    const int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    char *dest = arena_alloc(string_arena, len + 1);
    if (!dest) {
        va_end(args);
        return ARENA_FULL;
    }

    vsnprintf(dest, len + 1, format, args);
    va_end(args);

    *sv = (StringView){len, dest};
    return OK;
}

void sv_chop_left(StringView *sv, const size_t n) {
    const size_t chop_amount = n > sv->length ? sv->length : n;
    sv->chars += chop_amount;
    sv->length -= chop_amount;
}

bool sv_equals(const StringView sv1, const StringView sv2) {
    if (sv1.length != sv2.length) return false;

    for (size_t i = 0; i < sv1.length; ++i) {
        if (sv1.chars[i] != sv2.chars[i]) return false;
    }

    return true;
}

// Transfer ownership of the remaining memory in arena to the StringBuilder.
// Nothing else can alloc on the arena until the SB is released as the arena spare capacity is shrunk to zero.
StringBuilder sb_create_and_borrow_arena(Arena *arena) {
    const StringBuilder sb = {
        .arena = arena,
        .length = 0,
        .capacity = arena->capacity - arena->used,
        .chars = arena->data + arena->used,
    };
    arena->capacity = arena->used;
    return sb;
}

// Updates the arena with the memory the StringBuilder used to transfer ownership back.
StringView sb_complete_and_release_arena(StringBuilder *sb) {
    // The sb_ functions should check bounds and never allow this
    assert(sb->length <= sb->capacity);

    const StringView string = {sb->length, sb->chars};

    sb->arena->capacity += sb->capacity;
    sb->arena->used += sb->length;
    sb->capacity = 0;
    sb->length = 0;
    return string;
}

enum Error sb_append_char(StringBuilder *sb, const char c) {
    if (sb->length >= sb->capacity) return ARENA_FULL;
    sb->chars[sb->length++] = c;
    return OK;
}

enum Error sb_append_sv(StringBuilder *sb, const StringView sv) {
    if (sb->length + sv.length > sb->capacity) return ARENA_FULL;
    memcpy(sb->chars + sb->length, sv.chars, sv.length);
    sb->length += sv.length;
    return OK;
}

enum Error sb_append_formatted(StringBuilder *sb, const char *format, ...) __attribute__((format(printf, 2, 3)));
enum Error sb_append_formatted(StringBuilder *sb, const char *format, ...) {
    va_list args, args_copy;
    va_start(args, format);

    va_copy(args_copy, args);
    const int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    // We need space for the null-terminator produced by printf
    if (sb->length + len + 1 > sb->capacity) return ARENA_FULL;

    vsnprintf(sb->chars + sb->length, len + 1, format, args);
    va_end(args);

    // Don't include the null-terminator in the resulting string
    sb->length += len;
    return OK;
}

// ==================== Public JSON enums and structures ====================

enum JsonType { JSON_OBJECT, JSON_ARRAY, JSON_NUMBER, JSON_STRING, JSON_BOOLEAN, JSON_NULL };

typedef struct {
    Arena structure;
    Arena strings;
} JsonArena;

typedef struct JsonValue JsonValue;
typedef struct JsonMember JsonMember;

// Web APIs (e.g. AWS) typically have a small (< 50) number of possible names. A hashmap is not necessary, O(n) is fine.
typedef struct JsonObject {
    JsonMember *members;
    size_t count;
    size_t capacity;
} JsonObject;

typedef struct JsonArray {
    JsonValue *elements;
    size_t count;
    size_t capacity;
} JsonArray;

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
    String name;
    JsonValue value;
};

// ==================== Creating JSON structures ====================

void json_arena_destroy(JsonArena *arena) {
    arena_destroy(&arena->structure);
    arena_destroy(&arena->strings);
}

enum Error json_arena_create(JsonArena *arena, const size_t structure_size, const size_t strings_size) {
    *arena = (JsonArena){0};
    arena->structure = arena_create(structure_size, 8);
    if (!arena->structure.data) return MALLOC_FAIL;

    arena->strings = arena_create(strings_size, 1);
    if (!arena->strings.data) {
        json_arena_destroy(arena);
        return MALLOC_FAIL;
    }
    return OK;
}

JsonValue json_object(const JsonObject object) { return (JsonValue){JSON_OBJECT, .object = object}; }
JsonValue json_array(const JsonArray array) { return (JsonValue){JSON_ARRAY, .array = array}; }
JsonValue json_string(const StringView string) { return (JsonValue){JSON_STRING, .string = string}; }
JsonValue json_number(const double number) { return (JsonValue){JSON_NUMBER, .number = number}; }
JsonValue json_bool(const bool boolean) { return (JsonValue){JSON_BOOLEAN, .boolean = boolean}; }
JsonValue json_null() { return (JsonValue){JSON_NULL}; }

enum Error json_object_add_member(JsonArena *arena, JsonObject *object, const StringView name_sv,
                                  const JsonValue value) {
    if (object->capacity == 0) {
        const int initial_capacity = 4;

        object->members = arena_alloc(&arena->structure, initial_capacity * sizeof(JsonMember));
        if (!object->members) return ARENA_FULL;

        object->capacity = initial_capacity;
    } else if (object->count >= object->capacity) {
        const size_t old_size = object->capacity * sizeof(JsonMember);

        // Arena has not grown so we can expand the existing allocation
        if (arena->structure.data + arena->structure.used == object->members + object->count) {
            if (arena->structure.used + old_size > arena->structure.capacity) return ARENA_FULL;
            arena->structure.used += old_size;
            object->capacity *= 2;
        } else {
            JsonMember *new_members = arena_realloc(&arena->structure, object->members, old_size, 2 * old_size);
            if (!new_members) return ARENA_FULL;
            object->members = new_members;
            object->capacity *= 2;
        }
    }

    // We take an SV for the name, but we want to store the entire JSON together in the arena with ownership of it.
    String name;
    TRY(copy_sv_to_string(&arena->strings, name_sv, &name));

    object->members[object->count++] = (JsonMember){name, value};
    return OK;
}

enum Error json_object_add_member_s(JsonArena *arena, JsonObject *object, const char *name, const JsonValue value) {
    return json_object_add_member(arena, object, sv_on_c_string(name), value);
}

enum Error json_object_add_member_s_s(JsonArena *arena, JsonObject *object, const char *name, const char *value) {
    String str;
    copy_c_string_to_string(&arena->strings, value, &str);
    return json_object_add_member(arena, object, sv_on_c_string(name), json_string(sv_on_string(str)));
}

enum Error json_array_append(JsonArena *arena, JsonArray *array, const JsonValue value) {
    if (array->capacity == 0) {
        const int initial_capacity = 4;

        array->elements = arena_alloc(&arena->structure, initial_capacity * sizeof(JsonValue));
        if (!array->elements) return ARENA_FULL;

        array->capacity = 4;
    } else if (array->count >= array->capacity) {
        const size_t old_size = array->capacity * sizeof(JsonValue);

        // Arena has not grown so we can expand the existing allocation
        if (arena->structure.data + arena->structure.used == array->elements + array->count) {
            if (arena->structure.used + old_size > arena->structure.capacity) return ARENA_FULL;
            arena->structure.used += old_size;
            array->capacity *= 2;
        } else {
            JsonValue *new_elements = arena_realloc(&arena->structure, array->elements, old_size, 2 * old_size);
            if (!new_elements) return ARENA_FULL;
            array->elements = new_elements;
            array->capacity *= 2;
        }
    }

    array->elements[array->count++] = value;
    return OK;
}

// ==================== Lexing JSON ====================

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

static enum Error json_lex_next_number(Token *token, StringView *sv) {
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
                return OK;
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

static enum Error json_lex_next_string(Arena *string_arena, Token *token, StringView *sv) {
    if (*sv->chars != '"') return JSONE_PARSE_CHARACTER_UNEXPECTED;
    sv_chop_left(sv, 1);

    enum Error err;
    StringBuilder sb = sb_create_and_borrow_arena(string_arena);

    while (sv->length > 0) {
        if (*sv->chars == '"') {
            sv_chop_left(sv, 1);

            const StringView string = sb_complete_and_release_arena(&sb);
            *token = (Token){JSON_KIND_STRING, .string = string};

            DEFER(OK);
        }

        if (*sv->chars == '\\') {
            // Backslash escape cannot be cut off by EOF
            if (sv->length < 2) DEFER(JSONE_PARSE_BACKLASH_ESCAPE_INVALID);

            if (sv->chars[1] == 'u') {
                if (sv->length < 6) DEFER(JSONE_PARSE_HEX_ESCAPE_INVALID);

                for (int i = 2; i < 6; ++i) {
                    if (!isxdigit(sv->chars[i])) DEFER(JSONE_PARSE_HEX_ESCAPE_INVALID);
                }

                char buf[5] = {0};
                memcpy(buf, sv->chars + 2, 4);
                const long n = strtol(buf, NULL, 16);

                if (n < 0 || n >= 32) DEFER(JSONE_PARSE_HEX_ESCAPE_INVALID);

                TRY(sb_append_char(&sb, n));
                sv_chop_left(sv, 6);
            } else {
                // Valid escapes are "\bfnrt/
                const char c = PARSE_BACKSLASH_ESCAPE_MAP[sv->chars[1]];
                if (c == 0) DEFER(JSONE_PARSE_BACKLASH_ESCAPE_INVALID);

                TRY(sb_append_char(&sb, c));
                sv_chop_left(sv, 2);
            }
        } else if ((uint8_t)*sv->chars < 128) {
            // Control characters must be escaped and cannot be directly in strings
            if (*sv->chars < 32) DEFER(JSONE_PARSE_CONTROL_CHARACTER_UNESCAPED);

            TRY(sb_append_char(&sb, *sv->chars));
            sv_chop_left(sv, 1);
        } else {
            const uint8_t *s = (uint8_t *)sv->chars;
            int length = 0;
            uint32_t state = 0;
            while (length++ < sv->length) {
                if (!utf8_validate(&state, *s++)) break;
            }

            if (state == 0) {  // OK
                TRY(sb_append_sv(&sb, (StringView){length, sv->chars}));
                sv_chop_left(sv, length);
            } else {
                DEFER(JSONE_PARSE_UTF8_INVALID);
            }
        }
    }
    err = JSONE_PARSE_STRING_UNCLOSED;
defer:
    sb_complete_and_release_arena(&sb);
    return err;
}

static enum Error json_lex_next_token(Arena *string_arena, Token *token, StringView *sv) {
    while (sv->length > 0) {
        const uint8_t c = *sv->chars;
        switch (c) {
            case '[':
                *token = (Token){JSON_KIND_BEGIN_ARRAY};
                sv_chop_left(sv, 1);
                return OK;
            case '{':
                *token = (Token){JSON_KIND_BEGIN_OBJECT};
                sv_chop_left(sv, 1);
                return OK;
            case ']':
                *token = (Token){JSON_KIND_END_ARRAY};
                sv_chop_left(sv, 1);
                return OK;
            case '}':
                *token = (Token){JSON_KIND_END_OBJECT};
                sv_chop_left(sv, 1);
                return OK;
            case ':':
                *token = (Token){JSON_KIND_NAME_SEPARATOR};
                sv_chop_left(sv, 1);
                return OK;
            case ',':
                *token = (Token){JSON_KIND_VALUE_SEPARATOR};
                sv_chop_left(sv, 1);
                return OK;
            case '"':
                return json_lex_next_string(string_arena, token, sv);
            case 't':
                if (sv->length >= 4 && memcmp(sv->chars, "true", 4) == 0) {
                    *token = (Token){JSON_KIND_BOOLEAN, .boolean = true};
                    sv_chop_left(sv, 4);
                    return OK;
                }
                return JSONE_PARSE_CHARACTER_UNEXPECTED;
            case 'f':
                if (sv->length >= 5 && memcmp(sv->chars, "false", 5) == 0) {
                    *token = (Token){JSON_KIND_BOOLEAN, .boolean = false};
                    sv_chop_left(sv, 5);
                    return OK;
                }
                return JSONE_PARSE_CHARACTER_UNEXPECTED;
            case 'n':
                if (sv->length >= 4 && memcmp(sv->chars, "null", 4) == 0) {
                    *token = (Token){JSON_KIND_NULL};
                    sv_chop_left(sv, 4);
                    return OK;
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
    return OK;
}

// ==================== Parsing JSON ====================

static const int JSON_DEPTH_LIMIT = 32;

typedef struct JsonParserState {
    JsonArena *arena;
    StringView *json_sv;
    const size_t json_length;
} JsonParserState;

static enum Error json_parse_value(const JsonParserState parser, JsonValue *result, const Token *value_token,
                                   const int depth) {
    if (depth > JSON_DEPTH_LIMIT) return JSONE_PARSE_DEPTH_LIMIT_EXCEEDED;
    switch (value_token->kind) {
        case JSON_KIND_BEGIN_ARRAY: {
            JsonArray array = {0};
            bool first = true;
            while (1) {
                Token element_token;
                TRY(json_lex_next_token(&parser.arena->strings, &element_token, parser.json_sv));

                if (element_token.kind == JSON_KIND_END_ARRAY) {
                    // Empty array
                    if (first) break;
                    return JSONE_PARSE_TOKEN_UNEXPECTED;
                }
                first = false;

                JsonValue element_value;
                TRY(json_parse_value(parser, &element_value, &element_token, depth + 1));
                TRY(json_array_append(parser.arena, &array, element_value));

                Token seperator_token;
                TRY(json_lex_next_token(&parser.arena->strings, &seperator_token, parser.json_sv));

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
                TRY(json_lex_next_token(&parser.arena->strings, &name_token, parser.json_sv));

                if (name_token.kind == JSON_KIND_END_OBJECT) {
                    // Empty object
                    if (first) break;
                    return JSONE_PARSE_TOKEN_UNEXPECTED;
                }
                first = false;

                if (name_token.kind != JSON_KIND_STRING) { return JSONE_PARSE_OBJECT_NAME_MISSING; }

                Token name_seperator_token;
                TRY(json_lex_next_token(&parser.arena->strings, &name_seperator_token, parser.json_sv));
                if (name_seperator_token.kind != JSON_KIND_NAME_SEPARATOR) {
                    return JSONE_PARSE_NAME_SEPARATOR_MISSING;
                }

                Token member_token;
                TRY(json_lex_next_token(&parser.arena->strings, &member_token, parser.json_sv));

                JsonValue member_value;
                TRY(json_parse_value(parser, &member_value, &member_token, depth + 1));
                TRY(json_object_add_member(parser.arena, &object, name_token.string, member_value));

                Token value_seperator_token;
                TRY(json_lex_next_token(&parser.arena->strings, &value_seperator_token, parser.json_sv));

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
    return OK;
}

static enum Error json_parser_handle_error(const JsonParserState parser, StringView *error_message,
                                           const enum Error err) {
    if (error_message) {
        TRY(sv_create_formatted(&parser.arena->strings, error_message, "Error parsing JSON at byte %zu: %s",
                                parser.json_length - parser.json_sv->length, error_get_message(err)));
    }
    return err;
}

enum Error json_parse(JsonArena *arena, JsonValue **result, StringView *error_message, StringView json_sv) {
    const JsonParserState parser = {arena, &json_sv, json_sv.length};

    Token token;
    {
        const enum Error err = json_lex_next_token(&parser.arena->strings, &token, parser.json_sv);
        if (err != OK) return json_parser_handle_error(parser, error_message, err);
    }

    JsonValue *v = arena_alloc(&arena->structure, sizeof(JsonValue));
    if (!v) return ARENA_FULL;

    {
        const enum Error err = json_parse_value(parser, v, &token, 0);
        if (err != OK) return json_parser_handle_error(parser, error_message, err);
    }

    *result = v;
    return OK;
}

// ==================== Reading JSON structures ====================

const JsonValue *json_get(const JsonObject *object, const StringView name) {
    for (size_t i = 0; i < object->count; ++i) {
        if (sv_equals(name, sv_on_string(object->members[i].name))) return &object->members[i].value;
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

const JsonValue *json_get_s(const JsonObject *object, const char *name) {
    return json_get(object, sv_on_c_string(name));
}

const JsonObject *json_get_object_s(const JsonObject *object, const char *name) {
    return json_get_object(object, sv_on_c_string(name));
}
const JsonArray *json_get_array_s(const JsonObject *object, const char *name) {
    return json_get_array(object, sv_on_c_string(name));
}
const StringView *json_get_string_s(const JsonObject *object, const char *name) {
    return json_get_string(object, sv_on_c_string(name));
}
const double *json_get_number_s(const JsonObject *object, const char *name) {
    return json_get_number(object, sv_on_c_string(name));
}
const bool *json_get_bool_s(const JsonObject *object, const char *name) {
    return json_get_bool(object, sv_on_c_string(name));
}

// ==================== Generating JSON from a structure ====================

static enum Error json_generate_indentation(StringBuilder *sb, const int indent_width, const int depth) {
    TRY(sb_append_char(sb, '\n'));
    for (int i = 0; i < indent_width * depth; ++i) { TRY(sb_append_char(sb, ' ')); }
    return OK;
}

static enum Error json_generate_string(StringBuilder *sb, StringView sv) {
    TRY(sb_append_char(sb, '"'));
    while (sv.length > 0) {
        if ((uint8_t)*sv.chars < 128) {
            const uint8_t escaped = GENERATE_BACKSLASH_ESCAPE_MAP[*sv.chars];
            if (escaped) {
                TRY(sb_append_char(sb, '\\'));
                TRY(sb_append_char(sb, escaped));
            } else if (*sv.chars < 32) {
                TRY(sb_append_formatted(sb, "\\u%.4X", *sv.chars));
            } else {
                TRY(sb_append_char(sb, *sv.chars));
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
                TRY(sb_append_sv(sb, (StringView){length, sv.chars}));
                sv_chop_left(&sv, length);
            } else {
                return JSONE_GEN_UTF8_INVALID;
            }
        }
    }
    TRY(sb_append_char(sb, '"'));
    return OK;
}

static enum Error json_generate_inner(StringBuilder *sb, const JsonValue *value, const int pretty, const int depth) {
    if (depth > JSON_DEPTH_LIMIT) return JSONE_GEN_DEPTH_LIMIT_EXCEEDED;
    switch (value->type) {
        case JSON_OBJECT:
            TRY(sb_append_char(sb, '{'));
            for (size_t i = 0; i < value->object.count; ++i) {
                if (pretty > 0) TRY(json_generate_indentation(sb, pretty, depth + 1));

                const JsonMember member = value->object.members[i];

                TRY(json_generate_string(sb, sv_on_string(member.name)));
                TRY(sb_append_char(sb, ':'));

                // When pretty-formatting add a space after the colon
                if (pretty > 0) TRY(sb_append_char(sb, ' '));

                TRY(json_generate_inner(sb, &member.value, pretty, depth + 1));

                // Last in object has no comma
                if (i != value->object.count - 1) TRY(sb_append_char(sb, ','));
            }
            if (pretty > 0 && value->object.count > 0) TRY(json_generate_indentation(sb, pretty, depth));

            TRY(sb_append_char(sb, '}'));
            break;
        case JSON_ARRAY:
            TRY(sb_append_char(sb, '['));
            for (size_t i = 0; i < value->array.count; ++i) {
                if (pretty > 0) TRY(json_generate_indentation(sb, pretty, depth + 1));

                TRY(json_generate_inner(sb, value->array.elements + i, pretty, depth + 1));

                // Last in array has no comma
                if (i != value->array.count - 1) TRY(sb_append_char(sb, ','));
            }
            if (pretty > 0 && value->array.count > 0) TRY(json_generate_indentation(sb, pretty, depth));

            TRY(sb_append_char(sb, ']'));
            break;
        case JSON_NUMBER:
            if (!isfinite(value->number)) return JSONE_GEN_NUM_INVALID;
            return sb_append_formatted(sb, "%.*G", DBL_DECIMAL_DIG, value->number);
        case JSON_STRING:
            return json_generate_string(sb, value->string);
        case JSON_BOOLEAN:
            return sb_append_sv(sb, sv_on_c_string(value->boolean ? "true" : "false"));
        case JSON_NULL:
            return sb_append_sv(sb, sv_on_c_string("null"));
    }
    return OK;
}

enum Error json_generate(StringBuilder *sb, const JsonValue *value) { return json_generate_inner(sb, value, 0, 0); }

enum Error json_generate_pretty(StringBuilder *sb, const JsonValue *value, const int pretty_width) {
    return json_generate_inner(sb, value, pretty_width, 0);
}

// ==================== Miscellaneous ====================

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct MemMapFile {
    int fd;
    size_t file_size;
    char *data;
} MemMapFile;

MemMapFile open_mem_map_file(const char *path) {
    const int fd = open(path, O_RDONLY);
    if (fd == -1) { return (MemMapFile){0}; }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        return (MemMapFile){0};
    }

    const size_t fileSize = (size_t)st.st_size;

    char *data = mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);

    if (data == MAP_FAILED) {
        close(fd);
        return (MemMapFile){0};
    }

    return (MemMapFile){fd, fileSize, data};
}

void close_mem_map_file(const MemMapFile file) {
    munmap(file.data, file.file_size);
    close(file.fd);
}

double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// ==================== Tests ====================

// clang-format off
static const char *TEST_JSON_STRING = STR({"60":60,"61":61,"62":62,"63":63,"64":64,"65":65,"66":66,"67":67,"68":68,"69":69,"array":[true,false,null,3.1415926535897931,-3.1415926535897932E+30,{},[[]],"abc"],"Query":{"TableName":"MeasurementsTable","Limit":1,"KeyConditionExpression":"device_id = :d","ExpressionAttributeValues":{":pk":{"S":"picotherm/1"}},"ScanIndexForward":false},"Unicode character with 2 bytes":"π","Unicode character with 3 bytes":"歌","Unicode character with 4 bytes":"𝄞","Emoji":"⁶🤷‍♂️⁷","Reverse solidus escapes":"\" \\ \b \f \n \r \t","Must escape control characters":"\u001F \u001E \u001D \u001C \u001B \u001A \u0019 \u0018 \u0017 \u0016 \u0015 \u0014 \u0013 \u0012 \u0011 \u0010 \u000F \u000E \r \f \u000B \n \t \b \u0007 \u0006 \u0005 \u0004 \u0003 \u0002 \u0001 \u0000"});
// clang-format on

static void test_json_build_generate() {
    JsonArena arena;
    json_arena_create(&arena, 1024 * 1024, 1024 * 1024);

    JsonValue root_val = json_object((JsonObject){0});
    JsonObject *root_obj = &root_val.object;

    for (int i = 60; i < 70; ++i) {
        char number_as_string[8];
        sprintf(number_as_string, "%d", i);

        json_object_add_member_s(&arena, root_obj, number_as_string, json_number(i));
    }

    JsonArray array = {0};
    json_array_append(&arena, &array, json_bool(true));
    json_array_append(&arena, &array, json_bool(false));
    json_array_append(&arena, &array, json_null());
    json_array_append(&arena, &array, json_number(3.141592653589793238462643383279));
    json_array_append(&arena, &array, json_number(-3141592653589793238462643383279.141592653589793238462643383279));

    json_array_append(&arena, &array, json_object((JsonObject){0}));

    JsonArray array_of_array = {0};
    const JsonArray empty_array = {0};
    json_array_append(&arena, &array_of_array, json_array(empty_array));
    json_array_append(&arena, &array, json_array(array_of_array));

    String abc;
    copy_c_string_to_string(&arena.strings, "abc", &abc);
    json_array_append(&arena, &array, json_string(sv_on_string(abc)));

    json_object_add_member_s(&arena, root_obj, "array", json_array(array));

    {
        JsonObject query = {0};
        json_object_add_member_s_s(&arena, &query, "TableName", "MeasurementsTable");
        json_object_add_member_s(&arena, &query, "Limit", json_number(1));
        json_object_add_member_s_s(&arena, &query, "KeyConditionExpression", "device_id = :d");

        {
            JsonObject pk_ref = {0};
            {
                JsonObject pk = {0};
                json_object_add_member_s_s(&arena, &pk, "S", "picotherm/1");
                json_object_add_member_s(&arena, &pk_ref, ":pk", json_object(pk));
            }
            json_object_add_member_s(&arena, &query, "ExpressionAttributeValues", json_object(pk_ref));
        }

        json_object_add_member_s(&arena, &query, "ScanIndexForward", json_bool(false));
        json_object_add_member_s(&arena, root_obj, "Query", json_object(query));
    }

    json_object_add_member_s_s(&arena, root_obj, "Unicode character with 2 bytes", "π");
    json_object_add_member_s_s(&arena, root_obj, "Unicode character with 3 bytes", "歌");
    json_object_add_member_s_s(&arena, root_obj, "Unicode character with 4 bytes", "𝄞");
    json_object_add_member_s_s(&arena, root_obj, "Emoji", "⁶🤷‍♂️⁷");
    json_object_add_member_s_s(&arena, root_obj, "Reverse solidus escapes", "\" \\ \b \f \n \r \t");
    {
        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        for (char i = 31; i >= 0; i--) {
            sb_append_char(&sb, i);
            if (i > 0) sb_append_char(&sb, ' ');
        }

        const StringView sv = sb_complete_and_release_arena(&sb);
        json_object_add_member_s(&arena, root_obj, "Must escape control characters", json_string(sv));
    }

    LOG_INFO("Arena used %zu + %zu = %zu bytes", arena.structure.used, arena.strings.used,
             arena.structure.used + arena.strings.used);

    StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);

    const enum Error err = json_generate(&sb, &root_val);
    assert(err == OK);

    const StringView json_string = sb_complete_and_release_arena(&sb);

    LOG_INFO("Generated JSON: " SV_FMT, SV_ARG(json_string));

    assert(json_string.length == strlen(TEST_JSON_STRING));
    assert(memcmp(json_string.chars, TEST_JSON_STRING, json_string.length) == 0);

    const double *six_seven = json_get_number_s(root_obj, "67");
    assert(six_seven);
    assert(*six_seven == 67);

    {
        const JsonObject *query = json_get_object_s(root_obj, "Query");
        const JsonObject *attribute_values = json_get_object_s(query, "ExpressionAttributeValues");
        const JsonObject *pk_obj = json_get_object_s(attribute_values, ":pk");
        const StringView *device_id = json_get_string_s(pk_obj, "S");
        assert(sv_equals(sv_on_c_string("picotherm/1"), *device_id));
    }

    json_arena_destroy(&arena);
}

static const uint8_t INVALID_UTF_STRINGS[][6] = {
    {0b10100001},                                                 // continuation byte
    {0b11100110, 0b10100001},                                     // unfinished utf8-3
    {0b11100110, 0b10100001, 'a'},                                // invalid continuation
    {0b11100110, 0b10100001, 0b10000001, 0b10000001},             // too many con bytes
    {0xC0, 0xBF},                                                 // overlong
    {0xF4, 0x90, 0x80, 0x80},                                     // too big for Unicode
    {0b1111100, 0b10000001, 0b10000001, 0b10000001, 0b10000001},  // utf8-5
};

static void test_json_generate_string_invalid() {
    // The main tests cover utf8 characters already, this covers invalid utf8 detection.
    // mbrtoc32 is used for validation so we don't need to test the whole spec.

    Arena arena = arena_create(1024, 1);

    for (int i = 0; i < sizeof(INVALID_UTF_STRINGS) / sizeof(*INVALID_UTF_STRINGS); ++i) {
        StringBuilder sb = sb_create_and_borrow_arena(&arena);
        const enum Error err = json_generate_string(&sb, sv_on_c_string((char *)INVALID_UTF_STRINGS[i]));
        sb_complete_and_release_arena(&sb);
        assert(JSONE_GEN_UTF8_INVALID == err);
    }

    arena_destroy(&arena);
}

static void test_json_lexer_simple_tokens() {
    Arena arena;
    const char *text = "[] {}\r:,\ntrue\tfalse null";
    StringView sv = sv_on_c_string(text);
    Token t;

    enum Error err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_BEGIN_ARRAY);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_END_ARRAY);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_BEGIN_OBJECT);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_END_OBJECT);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_NAME_SEPARATOR);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_VALUE_SEPARATOR);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_BOOLEAN);
    assert(t.boolean == true);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_BOOLEAN);
    assert(t.boolean == false);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == OK);
    assert(t.kind == JSON_KIND_NULL);
}

static void test_json_lexer_numbers() {
    struct TestCaseSuccess {
        char *string;
        double expected_number;
        size_t expected_length_remaining;
    };

    const struct TestCaseSuccess success_cases[] = {
        // Valid JSON numbers
        {"0", 0, 0},
        {"-0", 0, 0},
        {"67", 67, 0},
        {"0.0", 0.0, 0},
        {"-0.0", -0.0, 0},
        {"3.14", 3.14, 0},
        {"1e9", 1e9, 0},
        {"6.02E+23", 6.02e+23, 0},
        {"6.02E+23 hello", 6.02e+23, 6},
        {"1.67e-27", 1.67e-27, 0},
        {"-123e45", -123e45, 0},
        // Parse double precision - no particular precision required by the RFC, but it is advised
        {"3.141592653589793238462643383279", 3.1415926535897931, 0},
        // Stops on end states - lexer or parser will fail on next token if invalid
        {"01", 0, 1},
        {"1A", 1, 1},
        {"3.14A", 3.14, 1},
        {"1e9e", 1e9, 1}};

    for (int i = 0; i < sizeof(success_cases) / sizeof(*success_cases); ++i) {
        struct TestCaseSuccess test = success_cases[i];
        StringView sv = sv_on_c_string(test.string);
        Token t;
        const enum Error err = json_lex_next_number(&t, &sv);
        assert(err == OK);
        assert(t.kind == JSON_KIND_NUMBER);
        assert(t.number == test.expected_number);
        assert(sv.length == test.expected_length_remaining);
    }

    const char fail_cases[][10] = {
        // Empty string
        "",
        // Starts with invalid character
        "X",
        " ",
        "*",
        "/",
        // Invalid starts that are valid elsewhere in numbers
        "+",
        "e9",
        ".25",
        // Invalid sign placements
        "-",
        "--1",
        "+-1",
        "-+1",
        // Malformed exponents
        "1e",
        "1e+",
        "1e-",
        "1eX",
        "1E+a",
        // Values representable in floating point but not JSON
        "NaN",
        "INF",
        "Infinity",
    };

    for (int i = 0; i < sizeof(fail_cases) / sizeof(*fail_cases); ++i) {
        StringView sv = sv_on_c_string(fail_cases[i]);
        Token t;
        const enum Error err = json_lex_next_number(&t, &sv);
        assert(err == JSONE_PARSE_CHARACTER_UNEXPECTED);
    }
}

static void test_json_lexer_strings() {
    Arena arena = arena_create(1024, 1);

    struct TestCaseSuccess {
        char *string;
        char *expected;
    };

    const struct TestCaseSuccess success_cases[] = {
        {"\"hello\"", "hello"},
        {"\"0123456789\"", "0123456789"},
        {"\"π 歌 𝄞 ⁶🤷‍♂️⁷ 👨‍👩‍👧‍👦🏴󠁧󠁢󠁥󠁮󠁧󠁿\"",
         "π 歌 𝄞 ⁶🤷‍♂️⁷ 👨‍👩‍👧‍👦🏴󠁧󠁢󠁥󠁮󠁧󠁿"},
        {"\"\\\" \\\\ \\b \\f \\n \\r \\t \\/\"", "\" \\ \b \f \n \r \t /"},
        {"\"\\u0001\"", "\1"},
    };

    for (int i = 0; i < sizeof(success_cases) / sizeof(*success_cases); ++i) {
        StringView sv = sv_on_c_string(success_cases[i].string);
        Token t;
        const enum Error err = json_lex_next_string(&arena, &t, &sv);
        assert(err == OK);
        assert(t.kind == JSON_KIND_STRING);
        assert(sv_equals(t.string, sv_on_c_string(success_cases[i].expected)));
    }

    struct TestCaseFail {
        char *string;
        enum Error expected;
    };

    const struct TestCaseFail fail_cases[] = {
        {"\"hello", JSONE_PARSE_STRING_UNCLOSED},
        {"\"\\", JSONE_PARSE_BACKLASH_ESCAPE_INVALID},
        {"\"\\x\"", JSONE_PARSE_BACKLASH_ESCAPE_INVALID},
        {"\"\b\"", JSONE_PARSE_CONTROL_CHARACTER_UNESCAPED},
        {"\"\x1\"", JSONE_PARSE_CONTROL_CHARACTER_UNESCAPED},
        {"\"\\u0020\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
        {"\"\\u001\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
        {"\"\\ua\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
    };

    for (int i = 0; i < sizeof(fail_cases) / sizeof(*fail_cases); ++i) {
        StringView sv = sv_on_c_string(fail_cases[i].string);
        Token t;
        const enum Error err = json_lex_next_string(&arena, &t, &sv);
        assert(fail_cases[i].expected == err);
    }

    for (int i = 0; i < sizeof(INVALID_UTF_STRINGS) / sizeof(*INVALID_UTF_STRINGS); ++i) {
        StringBuilder sb = sb_create_and_borrow_arena(&arena);
        sb_append_char(&sb, '"');
        sb_append_sv(&sb, sv_on_c_string((char *)INVALID_UTF_STRINGS[i]));
        sb_append_char(&sb, '"');
        sb_append_char(&sb, 0);
        StringView sv = sb_complete_and_release_arena(&sb);
        Token t;
        const enum Error err = json_lex_next_string(&arena, &t, &sv);
        assert(err == JSONE_PARSE_UTF8_INVALID);
    }

    arena_destroy(&arena);
}

static void test_json_parse() {
    JsonArena arena;
    json_arena_create(&arena, 1024 * 1024, 1024 * 1024);

    JsonValue *v;
    StringView error_message = {0};

    {
        StringView test_string = sv_on_c_string("true");
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == OK);
        assert(v->type == JSON_BOOLEAN);
        assert(v->boolean == true);
    }
    {
        StringView test_string = sv_on_c_string("\t null");
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == OK);
        assert(v->type == JSON_NULL);
    }
    {
        StringView test_string = sv_on_c_string("\n   67  ");
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == OK);
        assert(v->type == JSON_NUMBER);
        assert(v->number == 67);
    }
    {
        StringView test_string = sv_on_c_string("\"hello \"");
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == OK);
        assert(v->type == JSON_STRING);
        assert(sv_equals(v->string, sv_on_c_string("hello ")));
    }
    {
        StringView test_string = sv_on_c_string(STR([ 1, false, [], [[[[["hello"]]]]] ]));
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == OK);
        assert(v->type == JSON_ARRAY);

        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        json_generate(&sb, v);
        const StringView output_string = sb_complete_and_release_arena(&sb);
        assert(sv_equals(sv_on_c_string("[1,false,[],[[[[[\"hello\"]]]]]]"), output_string));
    }
    {
        StringView test_string = sv_on_c_string("[ ]");
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == OK);
        assert(v->type == JSON_ARRAY);

        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        json_generate(&sb, v);
        const StringView output_string = sb_complete_and_release_arena(&sb);
        assert(sv_equals(sv_on_c_string("[]"), output_string));
    }
    {
        StringView test_string = sv_on_c_string(STR(
            {"hello" : "world", "object" : {"nested" : 1}, "empty" : {}, "more nested" : {"" : {"" : [ {}, 67 ]}}}));
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == OK);
        assert(v->type == JSON_OBJECT);

        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        json_generate(&sb, v);
        const StringView output_string = sb_complete_and_release_arena(&sb);
        assert(sv_equals(
            sv_on_c_string(
                "{\"hello\":\"world\",\"object\":{\"nested\":1},\"empty\":{},\"more nested\":{\"\":{\"\":[{},67]}}}"),
            output_string));
    }
    {
        StringView test_string = sv_on_c_string(TEST_JSON_STRING);
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        if (err != OK) {
            assert(error_message.length > 0);
            LOG_ERROR(SV_FMT, SV_ARG(error_message));
            assert(0);
        }

        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        json_generate(&sb, v);
        const StringView output_string = sb_complete_and_release_arena(&sb);
        assert(sv_equals(sv_on_c_string(TEST_JSON_STRING), output_string));
    }
    {
        StringView test_string = sv_on_c_string("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]");
        const enum Error err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == OK);
    }

    json_arena_destroy(&arena);
}

static void test_json_parse_errors() {
    JsonArena arena;
    json_arena_create(&arena, 1024 * 1024, 1024 * 1024);

    JsonValue *v;
    StringView error_message = {0};
    enum Error err = 0;

    struct TestCase {
        char *input;
        enum Error expected_error;
    };

    const struct TestCase cases[] = {
        {"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]",
         JSONE_PARSE_DEPTH_LIMIT_EXCEEDED},
        {"invalid", JSONE_PARSE_CHARACTER_UNEXPECTED},
        {"[1 2]", JSONE_PARSE_ARRAY_SEPARATOR_MISSING},
        {"[}]", JSONE_PARSE_TOKEN_UNEXPECTED},
        {"[1,2,]", JSONE_PARSE_TOKEN_UNEXPECTED},
        {"[1,2,3[", JSONE_PARSE_ARRAY_SEPARATOR_MISSING},
        {STR({"a" : 1 "b" : 2}), JSONE_PARSE_OBJECT_SEPARATOR_MISSING},
        {"{]}", JSONE_PARSE_OBJECT_NAME_MISSING},
        {STR({
             "a" : 1,
             "b" : 2,
         }),
         JSONE_PARSE_TOKEN_UNEXPECTED},
        {STR({"a" 1}), JSONE_PARSE_NAME_SEPARATOR_MISSING}};

    for (int i = 0; i < sizeof(cases) / sizeof(struct TestCase); ++i) {
        error_message.length = 0;
        const StringView sv = sv_on_c_string(cases[i].input);
        err = json_parse(&arena, &v, &error_message, sv);
        if (err != cases[i].expected_error) {
            LOG_ERROR(SV_FMT " " SV_FMT, SV_ARG(sv), SV_ARG(error_message));
            assert(err == cases[i].expected_error);
        }
    }

    json_arena_destroy(&arena);
}

static int count_json_objects(const JsonValue v) {
    switch (v.type) {
        case JSON_OBJECT: {
            int sum = 0;
            for (int i = 0; i < v.object.count; ++i) { sum += count_json_objects(v.object.members[i].value); }
            return sum + 1;
        }
        case JSON_ARRAY: {
            int sum = 0;
            for (int i = 0; i < v.array.count; ++i) { sum += count_json_objects(v.array.elements[i]); }
            return sum;
        }
        default:
            return 0;
    }
}

// ReSharper disable once CppDFAConstantParameter
static void test_json_parse_twitter(const int test_runs) {
    const MemMapFile file = open_mem_map_file("../test_files/twitter.json");
    assert(file.data);

    JsonArena arena;
    json_arena_create(&arena, 2 * 1024 * 1024, 1024 * 1024);

    JsonValue *json_value;
    StringView error_message = {0};

    const double start = get_time();

    for (int i = 0; i < test_runs; ++i) {
        const enum Error err = json_parse(&arena, &json_value, &error_message, (StringView){file.file_size, file.data});
        LOG_INFO("Arena used %zu + %zu = %zu bytes", arena.structure.used, arena.strings.used,
                 arena.structure.used + arena.strings.used);
        if (err != OK) {
            LOG_ERROR(SV_FMT, SV_ARG(error_message));
            assert(0);
        }
        arena_clear(&arena.structure);
        arena_clear(&arena.strings);
    }

    // ReSharper disable once CppDFAConstantConditions
    if (test_runs > 1) {
        // 0.8 ms
        LOG_INFO("Parsed %zu bytes in average of %f ms", file.file_size, 1000 * (get_time() - start) / test_runs);
    }

    assert(count_json_objects(*json_value) == 1264);

    close_mem_map_file(file);
}

int main(int argc, char *argv[]) {
    test_json_build_generate();
    test_json_generate_string_invalid();

    test_json_lexer_simple_tokens();
    test_json_lexer_numbers();
    test_json_lexer_strings();

    test_json_parse();
    test_json_parse_errors();
    test_json_parse_twitter(1);
}

// TODO accept all valid hex codes
// TODO integer number types
// TODO null pointer handling?

// Valid code points: 0x0000 <= cp <= 0xD7FF and 0xE000 <= cp <= 0x10FFFF