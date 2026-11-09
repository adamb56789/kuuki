#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STR(...) #__VA_ARGS__

#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)

#define KiB(n) (n * 1024)
#define MiB(n) (KiB(n) * 1024)
#define GiB(n) (MiB(n) * 1024)

#define DEFER(code)                         \
    do {                                    \
        err = code;                         \
        LOG_ERROR("Caught error: %d", err); \
        goto defer;                         \
    } while (0)

enum JsonError { JSONE_OK = 0, JSONE_MEMORY, JSONE_INVALID_NUMBER, JSONE_DEPTH_LIMIT_EXCEEDED };

#define TRY(expr)                                   \
    do {                                            \
        enum JsonError try_err = (expr);            \
        if (try_err != JSONE_OK) {                  \
            LOG_ERROR("Caught error: %d", try_err); \
            return try_err;                         \
        }                                           \
    } while (0)

enum JsonType { JSON_OBJECT, JSON_ARRAY, JSON_NUMBER, JSON_STRING, JSON_BOOLEAN, JSON_NULL };

typedef struct {
    void *data;
    size_t alignment;
    size_t used;
    size_t capacity;
} Arena;

typedef struct {
    Arena structure;
    Arena strings;
} JsonArena;

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

void json_arena_destroy(JsonArena *arena) {
    arena_destroy(&arena->structure);
    arena_destroy(&arena->strings);
}

enum JsonError json_arena_create(JsonArena *arena, const size_t structure_size, const size_t strings_size) {
    *arena = (JsonArena){0};
    arena->structure = arena_create(structure_size, 8);
    if (!arena->structure.data) return JSONE_MEMORY;

    arena->strings = arena_create(strings_size, 1);
    if (!arena->strings.data) {
        json_arena_destroy(arena);
        return JSONE_MEMORY;
    }
    return JSONE_OK;
}

void *arena_alloc(Arena *arena, const size_t size) {
    const size_t offset = align_up(arena->used, arena->alignment);

    if (offset > arena->capacity || size > arena->capacity - offset) { return NULL; }

    void *ptr = (char *)arena->data + offset;
    arena->used = offset + size;

    return ptr;
}

void *arena_realloc(Arena *arena, const void *old_ptr, const size_t old_size, const size_t new_size) {
    assert(new_size > old_size);

    const size_t offset = align_up(arena->used, arena->alignment);

    if (offset > arena->capacity || new_size > arena->capacity - offset) return NULL;

    void *new_ptr = (char *)arena->data + offset;
    arena->used = offset + new_size;

    memcpy(new_ptr, old_ptr, old_size);
    return new_ptr;
}

typedef struct {
    size_t length;
    char *chars;
} StringView;

typedef struct {
    size_t length;
    char *chars;
} String;

typedef struct {
    size_t length;
    size_t capacity;
    char *chars;
} StringBuilder;

#define SV_FMT "%.*s"
#define SV_ARG(sv) (int)sv.length, sv.chars

StringView sv_on_string(const String string) { return (StringView){string.length, string.chars}; }

StringView sv_on_c_string(char *c_string) { return (StringView){strlen(c_string), c_string}; }

enum JsonError copy_sv_to_string(Arena *string_arena, const StringView sv, String *string) {
    string->chars = arena_alloc(string_arena, sv.length);
    if (!string->chars) return JSONE_MEMORY;

    memcpy(string->chars, sv.chars, sv.length);
    string->length = sv.length;
    return JSONE_OK;
}

enum JsonError copy_c_string_to_string(Arena *string_arena, char *c_string, String *string) {
    return copy_sv_to_string(string_arena, sv_on_c_string(c_string), string);
}

enum JsonError sb_append_char(StringBuilder *sb, const char c) {
    if (sb->length >= sb->capacity) return JSONE_MEMORY;
    sb->chars[sb->length++] = c;
    return JSONE_OK;
}

enum JsonError sb_append_sv(StringBuilder *sb, StringView sv) {
    if (sb->length + sv.length > sb->capacity) return JSONE_MEMORY;
    memcpy(sb->chars + sb->length, sv.chars, sv.length);
    sb->length += sv.length;
    return JSONE_OK;
}

bool sv_equals(const StringView sv1, const StringView sv2) {
    if (sv1.length != sv2.length) return false;

    for (size_t i = 0; i < sv1.length; ++i) {
        if (sv1.chars[i] != sv2.chars[i]) return false;
    }

    return true;
}

typedef struct JsonValue JsonValue;
typedef struct JsonMember JsonMember;

// Web APIs (e.g. AWS) typically have a small (< 50) number of possible names. A hashmap is not necessary, O(n) is fine.
typedef struct JsonObject {
    JsonMember *members;
    uint32_t count;
    uint32_t capacity;
} JsonObject;

typedef struct JsonArray {
    JsonValue *elements;
    uint32_t count;
    uint32_t capacity;
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

JsonValue json_object(const JsonObject object) { return (JsonValue){JSON_OBJECT, .object = object}; }
JsonValue json_array(const JsonArray array) { return (JsonValue){JSON_ARRAY, .array = array}; }
JsonValue json_string(const String string) { return (JsonValue){JSON_STRING, .string = string}; }
JsonValue json_number(const double number) { return (JsonValue){JSON_NUMBER, .number = number}; }
JsonValue json_bool(const bool boolean) { return (JsonValue){JSON_BOOLEAN, .boolean = boolean}; }
JsonValue json_null() { return (JsonValue){JSON_NULL}; }

enum JsonError json_object_add_member(JsonArena *arena, JsonObject *object, const StringView name_sv,
                                      const JsonValue value) {
    if (object->capacity == 0) {
        const int initial_capacity = 4;

        object->members = arena_alloc(&arena->structure, initial_capacity * sizeof(JsonMember));
        if (!object->members) return JSONE_MEMORY;

        object->capacity = initial_capacity;
    } else if (object->count >= object->capacity) {
        const size_t old_size = object->capacity * sizeof(JsonMember);

        // Arena has not grown so we can expand the existing allocation
        if (arena->structure.data + arena->structure.used == object->members + object->count) {
            if (arena->structure.used + old_size > arena->structure.capacity) return JSONE_MEMORY;
            arena->structure.used += old_size;
            object->capacity *= 2;
        } else {
            JsonMember *new_members = arena_realloc(&arena->structure, object->members, old_size, 2 * old_size);
            if (!new_members) return JSONE_MEMORY;
            object->members = new_members;
            object->capacity *= 2;
        }
    }

    // We take an SV for the name, but we want to store the entire JSON together in the arena with ownership of it.
    String name;
    TRY(copy_sv_to_string(&arena->strings, name_sv, &name));

    object->members[object->count++] = (JsonMember){name, value};
    return JSONE_OK;
}

enum JsonError json_object_add_member_s(JsonArena *arena, JsonObject *object, char *name, const JsonValue value) {
    return json_object_add_member(arena, object, sv_on_c_string(name), value);
}

enum JsonError json_object_add_member_s_s(JsonArena *arena, JsonObject *object, char *name, char *value) {
    String str;
    TRY(copy_c_string_to_string(&arena->strings, value, &str));
    return json_object_add_member(arena, object, sv_on_c_string(name), json_string(str));
}

enum JsonError json_array_append(JsonArena *arena, JsonArray *array, const JsonValue value) {
    if (array->capacity == 0) {
        const int initial_capacity = 4;

        array->elements = arena_alloc(&arena->structure, initial_capacity * sizeof(JsonValue));
        if (!array->elements) return JSONE_MEMORY;

        array->capacity = 4;
    } else if (array->count >= array->capacity) {
        const size_t old_size = array->capacity * sizeof(JsonValue);

        // Arena has not grown so we can expand the existing allocation
        if (arena->structure.data + arena->structure.used == array->elements + array->count) {
            if (arena->structure.used + old_size > arena->structure.capacity) return JSONE_MEMORY;
            arena->structure.used += old_size;
            array->capacity *= 2;
        } else {
            JsonValue *new_elements = arena_realloc(&arena->structure, array->elements, old_size, 2 * old_size);
            if (!new_elements) return JSONE_MEMORY;
            array->elements = new_elements;
            array->capacity *= 2;
        }
    }

    array->elements[array->count++] = value;
    return JSONE_OK;
}

const JsonMember *json_get(const JsonObject *object, const StringView name) {
    for (uint32_t i = 0; i < object->count; ++i) {
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

const JsonMember *json_get_s(const JsonObject *object, char *name) { return json_get(object, sv_on_c_string(name)); }

const JsonObject *json_get_object_s(const JsonObject *object, char *name) {
    return json_get_object(object, sv_on_c_string(name));
}
const JsonArray *json_get_array_s(const JsonObject *object, char *name) {
    return json_get_array(object, sv_on_c_string(name));
}
const String *json_get_string_s(const JsonObject *object, char *name) {
    return json_get_string(object, sv_on_c_string(name));
}
const double *json_get_number_s(const JsonObject *object, char *name) {
    return json_get_number(object, sv_on_c_string(name));
}
const bool *json_get_bool_s(const JsonObject *object, char *name) {
    return json_get_bool(object, sv_on_c_string(name));
}

static enum JsonError json_generate_indent(StringBuilder *sb, const int indent_width, const int depth) {
    TRY(sb_append_char(sb, '\n'));
    for (int i = 0; i < indent_width * depth; ++i) { TRY(sb_append_char(sb, ' ')); }
    return JSONE_OK;
}

static enum JsonError json_generate_number(StringBuilder *sb, const double number) {
    if (!isfinite(number)) return JSONE_INVALID_NUMBER;

    // Need temporary space because snprintf includes null-terminator
    // DBL_DECIMAL_DIG digits + decimal point + `E+XXX` + null-terminator
    char temp_number_buffer[32];
    snprintf(temp_number_buffer, sizeof(temp_number_buffer), "%.*G", DBL_DECIMAL_DIG, number);
    TRY(sb_append_sv(sb, sv_on_c_string(temp_number_buffer)));
    return JSONE_OK;
}

static const unsigned char BACKSLASH_ESCAPE_MAP[256] = {
    ['"'] = '"', ['\\'] = '\\', ['\b'] = 'b', ['\f'] = 'f', ['\n'] = 'n', ['\r'] = 'r', ['\t'] = 't',
};

static enum JsonError json_generate_u_escape(StringBuilder *sb, const unsigned value) {
    char buf[7];
    snprintf(buf, 7, "\\u%.4X", value);
    TRY(sb_append_sv(sb, sv_on_c_string(buf)));
    return JSONE_OK;
}

static enum JsonError json_generate_string(StringBuilder *sb, const StringView sv) {
    TRY(sb_append_char(sb, '"'));
    for (int i = 0; i < sv.length; ++i) {
        const unsigned char c = sv.chars[i];
        const unsigned char escaped = BACKSLASH_ESCAPE_MAP[c];
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
    return JSONE_OK;
}

static enum JsonError json_generate_inner(StringBuilder *sb, const JsonValue *value, const int pretty,
                                          const int depth) {
    if (depth >= 32) return JSONE_DEPTH_LIMIT_EXCEEDED;
    switch (value->type) {
        case JSON_OBJECT:
            TRY(sb_append_char(sb, '{'));
            for (uint32_t i = 0; i < value->object.count; ++i) {
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
            for (uint32_t i = 0; i < value->array.count; ++i) {
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
    return JSONE_OK;
}

enum JsonError json_generate(StringBuilder *sb, const JsonValue *value, const int pretty_width) {
    return json_generate_inner(sb, value, pretty_width, 0);
}

static void test_json_build_generate() {
    JsonArena arena;
    json_arena_create(&arena, MiB(1), MiB(1));

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
        char buf[100];
        StringBuilder sb = {0, 100, buf};
        for (char i = 31; i >= 0; i--) {
            sb_append_char(&sb, i);
            if (i > 0) sb_append_char(&sb, ' ');
        }
        const StringView sv = {sb.length, buf};
        String string;
        copy_sv_to_string(&arena.strings, sv, &string);
        json_object_add_member_s(&arena, root_obj, "Must escape control characters", json_string(string));
    }

    LOG_INFO("Arena used %zu + %zu = %zu bytes", arena.structure.used, arena.strings.used,
             arena.structure.used + arena.strings.used);

    // Let the StringBuilder allocate its own memory in the arena. Nothing else can alloc on the arena until returned.
    StringBuilder sb = {
        .length = 0,
        .capacity = arena.strings.capacity - arena.strings.used,
        .chars = arena.strings.data + arena.strings.used,
    };

    const enum JsonError err = json_generate(&sb, &root_val, 0);
    assert(err == JSONE_OK);
    // Update the arena with the memory the StringBuilder used.
    arena.strings.used += sb.length;

    LOG_INFO("Generated JSON: " SV_FMT, SV_ARG(sb));

    // clang-format off
    const char *expected = STR({"60":60,"61":61,"62":62,"63":63,"64":64,"65":65,"66":66,"67":67,"68":68,"69":69,"array":[true,false,null,3.1415926535897931,-3.1415926535897932E+30,{},[[]],"abc"],"Query":{"TableName":"MeasurementsTable","Limit":1,"KeyConditionExpression":"device_id = :d","ExpressionAttributeValues":{":pk":{"S":"picotherm/1"}},"ScanIndexForward":false},"Unicode character with 2 bytes":"π","Unicode character with 3 bytes":"歌","Unicode character with 4 bytes":"𝄞","Emoji":"⁶🤷‍♂️⁷","Reverse solidus escapes":"\" \\ \b \f \n \r \t","Must escape control characters":"\u001F \u001E \u001D \u001C \u001B \u001A \u0019 \u0018 \u0017 \u0016 \u0015 \u0014 \u0013 \u0012 \u0011 \u0010 \u000F \u000E \r \f \u000B \n \t \b \u0007 \u0006 \u0005 \u0004 \u0003 \u0002 \u0001 \u0000"});
    // clang-format on

    assert(sb.length == strlen(expected));
    assert(memcmp(sb.chars, expected, sb.length) == 0);

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

int main(int argc, char *argv[]) { test_json_build_generate(); }
// Validate utf8 in parser
// Diverge fom the spec and disallow \u escapes besides 0x00-0x1F
// Tape??