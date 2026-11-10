#include <assert.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== General macros ====================

#define STR(...) #__VA_ARGS__
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) fprintf(stderr, "[WARN] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)

enum KuukiError {
    KUUKIE_OK = 0,
    KUUKI_OOM,
    KUUKI_ARENA_FULL,
    JSONE_INVALID_NUMBER,
    JSONE_DEPTH_LIMIT_EXCEEDED,
    JSONE_NUMBER_LENGTH_LIMIT_EXCEEDED,
    JSONE_UNRECOGNIZED_TOKEN,
    JSONE_UNCLOSED_STRING,
    JSONE_INVALID_ESCAPE
};

#define TRY(expr)                                  \
    do {                                           \
        enum KuukiError try_err = (expr);          \
        if (try_err != KUUKIE_OK) {                \
            LOG_WARN("Caught error: %d", try_err); \
            return try_err;                        \
        }                                          \
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

enum KuukiError copy_sv_to_string(Arena *string_arena, const StringView sv, String *string) {
    string->chars = arena_alloc(string_arena, sv.length);
    if (!string->chars) return KUUKI_ARENA_FULL;

    memcpy(string->chars, sv.chars, sv.length);
    string->length = sv.length;
    return KUUKIE_OK;
}

enum KuukiError copy_c_string_to_string(Arena *string_arena, const char *c_string, String *string) {
    return copy_sv_to_string(string_arena, sv_on_c_string(c_string), string);
}

void sv_chop_left(StringView *sv, const size_t n) {
    const size_t chop_amount = n > sv->length ? sv->length : n;
    sv->chars += chop_amount;
    sv->length -= chop_amount;
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
String sb_complete_and_release_arena(StringBuilder *sb) {
    // The sb_ functions should check bounds and never allow this
    assert(sb->length <= sb->capacity);

    const String string = {sb->length, sb->chars};

    sb->arena->capacity += sb->capacity;
    sb->arena->used += sb->length;
    sb->capacity = 0;
    sb->length = 0;
    return string;
}

enum KuukiError sb_append_char(StringBuilder *sb, const char c) {
    if (sb->length >= sb->capacity) return KUUKI_ARENA_FULL;
    sb->chars[sb->length++] = c;
    return KUUKIE_OK;
}

enum KuukiError sb_append_sv(StringBuilder *sb, const StringView sv) {
    if (sb->length + sv.length > sb->capacity) return KUUKI_ARENA_FULL;
    memcpy(sb->chars + sb->length, sv.chars, sv.length);
    sb->length += sv.length;
    return KUUKIE_OK;
}

bool sv_equals(const StringView sv1, const StringView sv2) {
    if (sv1.length != sv2.length) return false;

    for (size_t i = 0; i < sv1.length; ++i) {
        if (sv1.chars[i] != sv2.chars[i]) return false;
    }

    return true;
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
        String string;
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

enum KuukiError json_arena_create(JsonArena *arena, const size_t structure_size, const size_t strings_size) {
    *arena = (JsonArena){0};
    arena->structure = arena_create(structure_size, 8);
    if (!arena->structure.data) return KUUKI_OOM;

    arena->strings = arena_create(strings_size, 1);
    if (!arena->strings.data) {
        json_arena_destroy(arena);
        return KUUKI_OOM;
    }
    return KUUKIE_OK;
}

JsonValue json_object(const JsonObject object) { return (JsonValue){JSON_OBJECT, .object = object}; }
JsonValue json_array(const JsonArray array) { return (JsonValue){JSON_ARRAY, .array = array}; }
JsonValue json_string(const String string) { return (JsonValue){JSON_STRING, .string = string}; }
JsonValue json_number(const double number) { return (JsonValue){JSON_NUMBER, .number = number}; }
JsonValue json_bool(const bool boolean) { return (JsonValue){JSON_BOOLEAN, .boolean = boolean}; }
JsonValue json_null() { return (JsonValue){JSON_NULL}; }

enum KuukiError json_object_add_member(JsonArena *arena, JsonObject *object, const StringView name_sv,
                                       const JsonValue value) {
    if (object->capacity == 0) {
        const int initial_capacity = 4;

        object->members = arena_alloc(&arena->structure, initial_capacity * sizeof(JsonMember));
        if (!object->members) return KUUKI_ARENA_FULL;

        object->capacity = initial_capacity;
    } else if (object->count >= object->capacity) {
        const size_t old_size = object->capacity * sizeof(JsonMember);

        // Arena has not grown so we can expand the existing allocation
        if (arena->structure.data + arena->structure.used == object->members + object->count) {
            if (arena->structure.used + old_size > arena->structure.capacity) return KUUKI_ARENA_FULL;
            arena->structure.used += old_size;
            object->capacity *= 2;
        } else {
            JsonMember *new_members = arena_realloc(&arena->structure, object->members, old_size, 2 * old_size);
            if (!new_members) return KUUKI_ARENA_FULL;
            object->members = new_members;
            object->capacity *= 2;
        }
    }

    // We take an SV for the name, but we want to store the entire JSON together in the arena with ownership of it.
    String name;
    TRY(copy_sv_to_string(&arena->strings, name_sv, &name));

    object->members[object->count++] = (JsonMember){name, value};
    return KUUKIE_OK;
}

enum KuukiError json_object_add_member_s(JsonArena *arena, JsonObject *object, const char *name,
                                         const JsonValue value) {
    return json_object_add_member(arena, object, sv_on_c_string(name), value);
}

enum KuukiError json_object_add_member_s_s(JsonArena *arena, JsonObject *object, const char *name, const char *value) {
    String str;
    TRY(copy_c_string_to_string(&arena->strings, value, &str));
    return json_object_add_member(arena, object, sv_on_c_string(name), json_string(str));
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
        String string;
        double number;
        bool boolean;
    };
} Token;

static enum KuukiError json_lex_next_number(Token *token, StringView *sv) {
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
                if (len == i) return JSONE_UNRECOGNIZED_TOKEN;
                if (isdigit(s[i])) {
                    if (s[i] != '0') {
                        state = LEADING_DIGIT;
                    } else {
                        state = LEADING_ZERO;
                    }
                } else if (s[i] == '-') {
                    state = AFTER_MINUS;
                } else {
                    return JSONE_UNRECOGNIZED_TOKEN;
                }
                break;
            case AFTER_MINUS:
                if (len == i) return JSONE_UNRECOGNIZED_TOKEN;
                if (isdigit(s[i])) {
                    if (s[i] != '0') {
                        state = LEADING_DIGIT;
                    } else {
                        state = LEADING_ZERO;
                    }
                } else {
                    return JSONE_UNRECOGNIZED_TOKEN;
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
                if (len == i) return JSONE_UNRECOGNIZED_TOKEN;
                if (isdigit(s[i])) {
                    state = EXPONENT_DIGITS;
                } else if (s[i] == '+' || s[i] == '-') {
                    state = EXPONENT_SIGN;
                } else {
                    return JSONE_UNRECOGNIZED_TOKEN;
                }
                break;
            case EXPONENT_SIGN:
                if (len == i) return JSONE_UNRECOGNIZED_TOKEN;
                if (isdigit(s[i])) {
                    state = EXPONENT_DIGITS;
                } else {
                    return JSONE_UNRECOGNIZED_TOKEN;
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
                char buf[64] = {0};
                memcpy(buf, sv->chars, i);

                char *end;
                *token = (Token){JSON_KIND_NUMBER, .number = strtod(buf, &end)};

                // If the state machine is correct strtod should parse the same length
                assert(end - buf == i);

                sv_chop_left(sv, i);
                return KUUKIE_OK;
            }
        }
        i++;
    }
}

static const char PARSE_BACKSLASH_ESCAPE_MAP[256] = {
    ['"'] = '"', ['\\'] = '\\', ['b'] = '\b', ['f'] = '\f', ['n'] = '\n', ['r'] = '\r', ['t'] = '\t',
};
static const char PARSE_BACKSLASH_ESCAPE_MAP_INVERSE[256] = {
    ['"'] = '"', ['\\'] = '\\', ['\b'] = 'b', ['\f'] = 'f', ['\n'] = 'n', ['\r'] = 'r', ['\t'] = 't',
};

static enum KuukiError json_lex_next_string(Arena *arena, Token *token, StringView *sv) {
    if (*sv->chars != '"') return JSONE_UNRECOGNIZED_TOKEN;
    sv_chop_left(sv, 1);

    enum KuukiError err;
    StringBuilder sb = sb_create_and_borrow_arena(arena);

    while (sv->length > 0) {
        if (*sv->chars == '"') {
            sv_chop_left(sv, 1);

            const String string = sb_complete_and_release_arena(&sb);
            *token = (Token){JSON_KIND_STRING, .string = string};

            return KUUKIE_OK;
        }

        if (*sv->chars == '\\') {
            sv_chop_left(sv, 1);
            if (sv->length == 0) DEFER(JSONE_INVALID_ESCAPE);

            const char c = PARSE_BACKSLASH_ESCAPE_MAP[*sv->chars];
            if (c == 0) DEFER(JSONE_INVALID_ESCAPE);

            sb_append_char(&sb, c);
            sv_chop_left(sv, 1);
        } else {
            sb_append_char(&sb, *sv->chars);
            sv_chop_left(sv, 1);
        }
    }
    err = JSONE_UNCLOSED_STRING;
defer:
    sb_complete_and_release_arena(&sb);
    return err;
}

static enum KuukiError json_lex_next_token(Arena *arena, Token *token, StringView *sv) {
    while (sv->length > 0) {
        const unsigned char c = *sv->chars;
        switch (c) {
            case '[':
                *token = (Token){JSON_KIND_BEGIN_ARRAY};
                sv_chop_left(sv, 1);
                return KUUKIE_OK;
            case '{':
                *token = (Token){JSON_KIND_BEGIN_OBJECT};
                sv_chop_left(sv, 1);
                return KUUKIE_OK;
            case ']':
                *token = (Token){JSON_KIND_END_ARRAY};
                sv_chop_left(sv, 1);
                return KUUKIE_OK;
            case '}':
                *token = (Token){JSON_KIND_END_OBJECT};
                sv_chop_left(sv, 1);
                return KUUKIE_OK;
            case ':':
                *token = (Token){JSON_KIND_NAME_SEPARATOR};
                sv_chop_left(sv, 1);
                return KUUKIE_OK;
            case ',':
                *token = (Token){JSON_KIND_VALUE_SEPARATOR};
                sv_chop_left(sv, 1);
                return KUUKIE_OK;
            case '"':
                json_lex_next_string(arena, token, sv);
                return KUUKIE_OK;
            case 't':
                if (sv->length >= 4 && memcmp(sv->chars, "true", 4) == 0) {
                    *token = (Token){JSON_KIND_BOOLEAN, .boolean = true};
                    sv_chop_left(sv, 4);
                    return KUUKIE_OK;
                }
                return JSONE_UNRECOGNIZED_TOKEN;
            case 'f':
                if (sv->length >= 5 && memcmp(sv->chars, "false", 5) == 0) {
                    *token = (Token){JSON_KIND_BOOLEAN, .boolean = false};
                    sv_chop_left(sv, 5);
                    return KUUKIE_OK;
                }
                return JSONE_UNRECOGNIZED_TOKEN;
            case 'n':
                if (sv->length >= 4 && memcmp(sv->chars, "null", 4) == 0) {
                    *token = (Token){JSON_KIND_NULL};
                    sv_chop_left(sv, 4);
                    return KUUKIE_OK;
                }
                return JSONE_UNRECOGNIZED_TOKEN;
            default:
                if (isspace(c)) {
                    sv_chop_left(sv, 1);
                } else if (isdigit(c) || c == '-') {
                    return json_lex_next_number(token, sv);
                } else {
                    return JSONE_UNRECOGNIZED_TOKEN;
                }
        }
    }

    *token = (Token){JSON_KIND_EOF};
    return KUUKIE_OK;
}

enum KuukiError json_array_append(JsonArena *arena, JsonArray *array, const JsonValue value) {
    if (array->capacity == 0) {
        const int initial_capacity = 4;

        array->elements = arena_alloc(&arena->structure, initial_capacity * sizeof(JsonValue));
        if (!array->elements) return KUUKI_ARENA_FULL;

        array->capacity = 4;
    } else if (array->count >= array->capacity) {
        const size_t old_size = array->capacity * sizeof(JsonValue);

        // Arena has not grown so we can expand the existing allocation
        if (arena->structure.data + arena->structure.used == array->elements + array->count) {
            if (arena->structure.used + old_size > arena->structure.capacity) return KUUKI_ARENA_FULL;
            arena->structure.used += old_size;
            array->capacity *= 2;
        } else {
            JsonValue *new_elements = arena_realloc(&arena->structure, array->elements, old_size, 2 * old_size);
            if (!new_elements) return KUUKI_ARENA_FULL;
            array->elements = new_elements;
            array->capacity *= 2;
        }
    }

    array->elements[array->count++] = value;
    return KUUKIE_OK;
}

// ==================== Reading JSON structures ====================

const JsonMember *json_get(const JsonObject *object, const StringView name) {
    for (size_t i = 0; i < object->count; ++i) {
        if (sv_equals(name, sv_on_string(object->members[i].name))) return &object->members[i];
    }
    return NULL;
}

const JsonObject *json_get_object(const JsonObject *object, const StringView name) {
    const JsonMember *m = json_get(object, name);
    if (!m) return NULL;
    if (m->value.type == JSON_OBJECT) return &m->value.object;
    return NULL;
}
const JsonArray *json_get_array(const JsonObject *object, const StringView name) {
    const JsonMember *m = json_get(object, name);
    if (!m) return NULL;
    if (m->value.type == JSON_ARRAY) return &m->value.array;
    return NULL;
}
const String *json_get_string(const JsonObject *object, const StringView name) {
    const JsonMember *m = json_get(object, name);
    if (!m) return NULL;
    if (m->value.type == JSON_STRING) return &m->value.string;
    return NULL;
}
const double *json_get_number(const JsonObject *object, const StringView name) {
    const JsonMember *m = json_get(object, name);
    if (!m) return NULL;
    if (m->value.type == JSON_NUMBER) return &m->value.number;
    return NULL;
}
const bool *json_get_bool(const JsonObject *object, const StringView name) {
    const JsonMember *m = json_get(object, name);
    if (!m) return NULL;
    if (m->value.type == JSON_BOOLEAN) return &m->value.boolean;
    return NULL;
}

const JsonMember *json_get_s(const JsonObject *object, const char *name) {
    return json_get(object, sv_on_c_string(name));
}

const JsonObject *json_get_object_s(const JsonObject *object, const char *name) {
    return json_get_object(object, sv_on_c_string(name));
}
const JsonArray *json_get_array_s(const JsonObject *object, const char *name) {
    return json_get_array(object, sv_on_c_string(name));
}
const String *json_get_string_s(const JsonObject *object, const char *name) {
    return json_get_string(object, sv_on_c_string(name));
}
const double *json_get_number_s(const JsonObject *object, const char *name) {
    return json_get_number(object, sv_on_c_string(name));
}
const bool *json_get_bool_s(const JsonObject *object, const char *name) {
    return json_get_bool(object, sv_on_c_string(name));
}

// ==================== Generating JSON from a structure ====================

static enum KuukiError json_generate_indent(StringBuilder *sb, const int indent_width, const int depth) {
    TRY(sb_append_char(sb, '\n'));
    for (int i = 0; i < indent_width * depth; ++i) { TRY(sb_append_char(sb, ' ')); }
    return KUUKIE_OK;
}

static enum KuukiError json_generate_number(StringBuilder *sb, const double number) {
    if (!isfinite(number)) return JSONE_INVALID_NUMBER;

    // Need temporary space because snprintf includes null-terminator
    // DBL_DECIMAL_DIG digits + decimal point + `E+XXX` + null-terminator
    char temp_number_buffer[32];
    snprintf(temp_number_buffer, sizeof(temp_number_buffer), "%.*G", DBL_DECIMAL_DIG, number);
    TRY(sb_append_sv(sb, sv_on_c_string(temp_number_buffer)));
    return KUUKIE_OK;
}

static enum KuukiError json_generate_u_escape(StringBuilder *sb, const unsigned value) {
    char buf[7];
    snprintf(buf, 7, "\\u%.4X", value);
    TRY(sb_append_sv(sb, sv_on_c_string(buf)));
    return KUUKIE_OK;
}

static enum KuukiError json_generate_string(StringBuilder *sb, const StringView sv) {
    TRY(sb_append_char(sb, '"'));
    for (int i = 0; i < sv.length; ++i) {
        const unsigned char c = sv.chars[i];
        const unsigned char escaped = PARSE_BACKSLASH_ESCAPE_MAP_INVERSE[c];
        if (!escaped) {
            if (c >= 32) {  // Most likely (normal ascii character) first
                TRY(sb_append_char(sb, c));
            } else {
                TRY(json_generate_u_escape(sb, c));
            }
        } else {
            TRY(sb_append_char(sb, '\\'));
            TRY(sb_append_char(sb, escaped));
        }
    }
    TRY(sb_append_char(sb, '"'));
    return KUUKIE_OK;
}

static enum KuukiError json_generate_inner(StringBuilder *sb, const JsonValue *value, const int pretty,
                                           const int depth) {
    if (depth >= 32) return JSONE_DEPTH_LIMIT_EXCEEDED;
    switch (value->type) {
        case JSON_OBJECT:
            TRY(sb_append_char(sb, '{'));
            for (size_t i = 0; i < value->object.count; ++i) {
                if (pretty > 0) TRY(json_generate_indent(sb, pretty, depth + 1));

                const JsonMember member = value->object.members[i];

                TRY(json_generate_string(sb, sv_on_string(member.name)));
                TRY(sb_append_char(sb, ':'));

                // When pretty-formatting add a space after the colon
                if (pretty > 0) TRY(sb_append_char(sb, ' '));

                TRY(json_generate_inner(sb, &member.value, pretty, depth + 1));

                // Last in object has no comma
                if (i != value->object.count - 1) TRY(sb_append_char(sb, ','));
            }
            if (pretty > 0 && value->object.count > 0) TRY(json_generate_indent(sb, pretty, depth));

            TRY(sb_append_char(sb, '}'));
            break;
        case JSON_ARRAY:
            TRY(sb_append_char(sb, '['));
            for (size_t i = 0; i < value->array.count; ++i) {
                if (pretty > 0) TRY(json_generate_indent(sb, pretty, depth + 1));

                TRY(json_generate_inner(sb, value->array.elements + i, pretty, depth + 1));

                // Last in array has no comma
                if (i != value->array.count - 1) TRY(sb_append_char(sb, ','));
            }
            if (pretty > 0 && value->array.count > 0) TRY(json_generate_indent(sb, pretty, depth));

            TRY(sb_append_char(sb, ']'));
            break;
        case JSON_NUMBER:
            TRY(json_generate_number(sb, value->number));
            break;
        case JSON_STRING:
            TRY(json_generate_string(sb, sv_on_string(value->string)));
            break;
        case JSON_BOOLEAN:
            TRY(sb_append_sv(sb, sv_on_c_string(value->boolean ? "true" : "false")));
            break;
        case JSON_NULL:
            TRY(sb_append_sv(sb, sv_on_c_string("null")));
            break;
    }
    return KUUKIE_OK;
}

enum KuukiError json_generate(StringBuilder *sb, const JsonValue *value, const int pretty_width) {
    return json_generate_inner(sb, value, pretty_width, 0);
}

// ==================== Tests ====================

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
    json_array_append(&arena, &array, json_string(abc));

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

        const String string = sb_complete_and_release_arena(&sb);
        json_object_add_member_s(&arena, root_obj, "Must escape control characters", json_string(string));
    }

    LOG_INFO("Arena used %zu + %zu = %zu bytes", arena.structure.used, arena.strings.used,
             arena.structure.used + arena.strings.used);

    StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);

    const enum KuukiError err = json_generate(&sb, &root_val, 0);
    assert(err == KUUKIE_OK);

    String json_string = sb_complete_and_release_arena(&sb);

    LOG_INFO("Generated JSON: " SV_FMT, SV_ARG(json_string));

    // clang-format off
    const char *expected = STR({"60":60,"61":61,"62":62,"63":63,"64":64,"65":65,"66":66,"67":67,"68":68,"69":69,"array":[true,false,null,3.1415926535897931,-3.1415926535897932E+30,{},[[]],"abc"],"Query":{"TableName":"MeasurementsTable","Limit":1,"KeyConditionExpression":"device_id = :d","ExpressionAttributeValues":{":pk":{"S":"picotherm/1"}},"ScanIndexForward":false},"Unicode character with 2 bytes":"π","Unicode character with 3 bytes":"歌","Unicode character with 4 bytes":"𝄞","Emoji":"⁶🤷‍♂️⁷","Reverse solidus escapes":"\" \\ \b \f \n \r \t","Must escape control characters":"\u001F \u001E \u001D \u001C \u001B \u001A \u0019 \u0018 \u0017 \u0016 \u0015 \u0014 \u0013 \u0012 \u0011 \u0010 \u000F \u000E \r \f \u000B \n \t \b \u0007 \u0006 \u0005 \u0004 \u0003 \u0002 \u0001 \u0000"});
    // clang-format on

    assert(json_string.length == strlen(expected));
    assert(memcmp(json_string.chars, expected, json_string.length) == 0);

    const double *six_seven = json_get_number_s(root_obj, "67");
    assert(six_seven);
    assert(*six_seven == 67);

    {
        const JsonObject *query = json_get_object_s(root_obj, "Query");
        const JsonObject *attribute_values = json_get_object_s(query, "ExpressionAttributeValues");
        const JsonObject *pk_obj = json_get_object_s(attribute_values, ":pk");
        const String *pk_string = json_get_string_s(pk_obj, "S");
        const StringView device_id = sv_on_string(*pk_string);
        assert(sv_equals(sv_on_c_string("picotherm/1"), device_id));
    }

    json_arena_destroy(&arena);
}

static void test_json_lexer_simple_tokens() {
    Arena arena;
    const char *text = "[] {}\r:,\ntrue\tfalse null";
    StringView sv = sv_on_c_string(text);
    Token t;

    enum KuukiError err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_BEGIN_ARRAY);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_END_ARRAY);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_BEGIN_OBJECT);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_END_OBJECT);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_NAME_SEPARATOR);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_VALUE_SEPARATOR);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_BOOLEAN);
    assert(t.boolean == true);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_BOOLEAN);
    assert(t.boolean == false);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == KUUKIE_OK);
    assert(t.kind == JSON_KIND_NULL);
}

#define ASSERT_LEX_NUMBER(s, x)          \
    sv = sv_on_c_string(s);              \
    err = json_lex_next_number(&t, &sv); \
    assert(err == KUUKIE_OK);            \
    assert(t.kind == JSON_KIND_NUMBER);  \
    assert(t.number == x)

#define ASSERT_LEX_NUMBER_FAIL(s)        \
    sv = sv_on_c_string(s);              \
    err = json_lex_next_number(&t, &sv); \
    assert(err == JSONE_UNRECOGNIZED_TOKEN)

static void test_json_lexer_numbers() {
    StringView sv;
    Token t;
    enum KuukiError err = 0;

    // Valid JSON numbers
    ASSERT_LEX_NUMBER("0", 0);
    ASSERT_LEX_NUMBER("-0", 0);
    ASSERT_LEX_NUMBER("67", 67);
    ASSERT_LEX_NUMBER("0.0", 0.0);
    ASSERT_LEX_NUMBER("-0.0", -0.0);
    ASSERT_LEX_NUMBER("3.14", 3.14);
    ASSERT_LEX_NUMBER("1e9", 1e9);
    ASSERT_LEX_NUMBER("6.02E+23", 6.02e+23);
    ASSERT_LEX_NUMBER("1.67e-27", 1.67e-27);
    ASSERT_LEX_NUMBER("-123e45", -123e45);

    // Parse double precision - no particular precision required by the RFC but it is advised
    ASSERT_LEX_NUMBER("3.141592653589793238462643383279", 3.1415926535897931);

    // Empty string
    ASSERT_LEX_NUMBER_FAIL("");

    // Starts with invalid character
    ASSERT_LEX_NUMBER_FAIL("X");
    ASSERT_LEX_NUMBER_FAIL(" ");
    ASSERT_LEX_NUMBER_FAIL("*");
    ASSERT_LEX_NUMBER_FAIL("/");

    // Invalid starts that are valid elsewhere in numbers
    ASSERT_LEX_NUMBER_FAIL("+");
    ASSERT_LEX_NUMBER_FAIL("e9");
    ASSERT_LEX_NUMBER_FAIL(".25");

    // Invalid sign placements
    ASSERT_LEX_NUMBER_FAIL("-");
    ASSERT_LEX_NUMBER_FAIL("--1");
    ASSERT_LEX_NUMBER_FAIL("+-1");
    ASSERT_LEX_NUMBER_FAIL("-+1");

    // Malformed exponents
    ASSERT_LEX_NUMBER_FAIL("1e");
    ASSERT_LEX_NUMBER_FAIL("1e+");
    ASSERT_LEX_NUMBER_FAIL("1e-");
    ASSERT_LEX_NUMBER_FAIL("1eX");
    ASSERT_LEX_NUMBER_FAIL("1E+a");

    // Values representable in floating point but not JSON
    ASSERT_LEX_NUMBER_FAIL("NaN");
    ASSERT_LEX_NUMBER_FAIL("INF");
    ASSERT_LEX_NUMBER_FAIL("Infinity");

    // Stops on end states - lexer or parser will fail on next token if invalid
    ASSERT_LEX_NUMBER("01", 0);
    assert(sv.length == 1);
    ASSERT_LEX_NUMBER("1A", 1);
    assert(sv.length == 1);
    ASSERT_LEX_NUMBER("3.14A", 3.14);
    assert(sv.length == 1);
    ASSERT_LEX_NUMBER("1e9e", 1e9);
    assert(sv.length == 1);
}

#define ASSERT_LEX_STRING(s, expected)           \
    sv = sv_on_c_string(s);                      \
    err = json_lex_next_string(&arena, &t, &sv); \
    assert(err == KUUKIE_OK);                    \
    assert(t.kind == JSON_KIND_STRING);          \
    assert(sv_equals(sv_on_string(t.string), sv_on_c_string(expected)));

#define ASSERT_LEX_STRING_FAIL(s, e)             \
    sv = sv_on_c_string(s);                      \
    err = json_lex_next_string(&arena, &t, &sv); \
    assert(err == e)

static void test_json_lexer_strings() {
    Arena arena = arena_create(1024, 1);
    StringView sv;
    Token t;
    enum KuukiError err = 0;

    ASSERT_LEX_STRING("\"hello\"", "hello");
    ASSERT_LEX_STRING("\"0123456789\"", "0123456789");
    ASSERT_LEX_STRING("\"π 歌 𝄞 ⁶🤷‍♂️⁷ 👨‍👩‍👧‍👦🏴󠁧󠁢󠁥󠁮󠁧󠁿\"", "π 歌 𝄞 ⁶🤷‍♂️⁷ 👨‍👩‍👧‍👦🏴󠁧󠁢󠁥󠁮󠁧󠁿");
    ASSERT_LEX_STRING("\"\\\" \\\\ \\b \\f \\n \\r \\t\"", "\" \\ \b \f \n \r \t");

    ASSERT_LEX_STRING_FAIL("\"hello", JSONE_UNCLOSED_STRING);
    ASSERT_LEX_STRING_FAIL("\"\\", JSONE_INVALID_ESCAPE);
    ASSERT_LEX_STRING_FAIL("\"\\x\"", JSONE_INVALID_ESCAPE);
}

int main(int argc, char *argv[]) {
    test_json_build_generate();
    test_json_lexer_simple_tokens();
    test_json_lexer_numbers();
    test_json_lexer_strings();
}
// Validate utf8 in parser ?
// Diverge fom the RFC and disallow \u escapes besides 0x00-0x1F ?
// Tape??