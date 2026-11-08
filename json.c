#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

enum JsonError { JSONE_OK = 0, JSONE_MEMORY, JSONE_INVALID_NUMBER };

#define TRY(expr)                               \
    do {                                        \
        enum JsonError err = (expr);            \
        if (err != JSONE_OK) {                  \
            LOG_ERROR("Caught error: %d", err); \
            return err;                         \
        }                                       \
    } while (0)

enum JsonType { JSON_OBJECT, JSON_ARRAY, JSON_NUMBER, JSON_STRING, JSON_BOOLEAN, JSON_NULL };

typedef struct {
    void *data;
    size_t used;
    size_t capacity;
} Arena;

static size_t align_up(const size_t x) { return x + (_Alignof(max_align_t) - 1) & ~(_Alignof(max_align_t) - 1); }

Arena arena_create(const size_t capacity) {
    void *data = aligned_alloc(_Alignof(max_align_t), align_up(capacity));

    if (!data) return (Arena){0};

    return (Arena){data, 0, capacity};
}

void arena_destroy(Arena *arena) {
    free(arena->data);
    arena->capacity = 0;
    arena->used = 0;
}

void *arena_alloc(Arena *arena, const size_t size) {
    const size_t offset = align_up(arena->used);

    if (offset > arena->capacity || size > arena->capacity - offset) { return NULL; }

    void *ptr = arena->data + offset;
    arena->used = offset + size;

    memset(ptr, 0, size);
    return ptr;
}

void *arena_realloc(Arena *arena, const void *old_ptr, const size_t old_size, const size_t new_size) {
    assert(new_size > old_size);

    const size_t offset = align_up(arena->used);

    if (offset > arena->capacity || new_size > arena->capacity - offset) return NULL;

    void *new_ptr = arena->data + offset;
    arena->used = offset + new_size;

    memcpy(new_ptr, old_ptr, old_size);
    memset(new_ptr + old_size, 0, new_size - old_size);
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

StringView take_sv_on_string(const String string) { return (StringView){string.length, string.chars}; }

StringView take_sv_on_c_string(char *c_string) { return (StringView){strlen(c_string), c_string}; }

enum JsonError copy_sv_to_string(Arena *arena, const StringView sv, String *string) {
    string->chars = arena_alloc(arena, sv.length);
    if (!string->chars) return JSONE_MEMORY;

    memcpy(string->chars, sv.chars, sv.length);
    string->length = sv.length;
    return JSONE_OK;
}

enum JsonError copy_c_string_to_string(Arena *arena, char *c_string, String *string) {
    return copy_sv_to_string(arena, take_sv_on_c_string(c_string), string);
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
typedef struct {
    JsonMember *members;
    size_t count;
    size_t capacity;
} JsonObject;

typedef struct {
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

typedef struct {
    Arena arena;
    JsonValue *root;
} JsonDocument;

JsonDocument json_document_init(const size_t capacity) {
    void *data = aligned_alloc(_Alignof(max_align_t), align_up(capacity));

    if (!data) return (JsonDocument){0};

    const Arena arena = {data, 0, capacity};
    return (JsonDocument){arena, NULL};
}

JsonValue json_object(const JsonObject object) { return (JsonValue){JSON_OBJECT, .object = object}; }
JsonValue json_array(const JsonArray array) { return (JsonValue){JSON_ARRAY, .array = array}; }
JsonValue json_string(const String string) { return (JsonValue){JSON_STRING, .string = string}; }
JsonValue json_number(const double number) { return (JsonValue){JSON_NUMBER, .number = number}; }
JsonValue json_bool(const bool boolean) { return (JsonValue){JSON_BOOLEAN, .boolean = boolean}; }
JsonValue json_null() { return (JsonValue){JSON_NULL}; }

enum JsonError json_object_add_member(Arena *arena, JsonObject *object, const StringView name_sv,
                                      const JsonValue value) {
    if (object->capacity == 0) {
        const size_t initial_capacity = 4;

        object->members = arena_alloc(arena, initial_capacity * sizeof(JsonMember));
        if (!object->members) return JSONE_MEMORY;

        object->capacity = initial_capacity;
    } else if (object->count >= object->capacity) {
        const size_t old_size = object->capacity * sizeof(JsonMember);

        object->members = arena_realloc(arena, object->members, old_size, 2 * old_size);
        if (!object->members) return JSONE_MEMORY;

        object->capacity *= 2;
    }

    // We take an SV for the name, but we want to store the entire JSON together in the arena with ownership of it.
    String name;
    TRY(copy_sv_to_string(arena, name_sv, &name));

    object->members[object->count++] = (JsonMember){name, value};
    return JSONE_OK;
}

enum JsonError json_object_add_member_s(Arena *arena, JsonObject *object, char *name, const JsonValue value) {
    return json_object_add_member(arena, object, take_sv_on_c_string(name), value);
}

enum JsonError json_object_add_member_s_s(Arena *arena, JsonObject *object, char *name, char *value) {
    String str;
    copy_c_string_to_string(arena, value, &str);
    return json_object_add_member(arena, object, take_sv_on_c_string(name), json_string(str));
}

enum JsonError json_array_append(Arena *arena, JsonArray *array, const JsonValue value) {
    if (array->capacity == 0) {
        const size_t initial_capacity = 4;

        array->elements = arena_alloc(arena, initial_capacity * sizeof(JsonValue));
        if (!array->elements) return JSONE_MEMORY;

        array->capacity = 4;
    } else if (array->count >= array->capacity) {
        const size_t old_size = array->capacity * sizeof(JsonValue);

        array->elements = arena_realloc(arena, array->elements, old_size, 2 * old_size);
        if (!array->elements) return JSONE_MEMORY;

        array->capacity *= 2;
    }

    array->elements[array->count++] = value;
    return JSONE_OK;
}

JsonMember *json_object_get(const JsonObject *object, const StringView name) {
    for (size_t i = 0; i < object->count; ++i) {
        const StringView member_name = take_sv_on_string(object->members[i].name);
        if (sv_equals(name, member_name)) return &object->members[i];
    }
    return NULL;
}

JsonMember *json_object_get_s(const JsonObject *object, char *name) {
    return json_object_get(object, take_sv_on_c_string(name));
}

enum JsonError json_generate_indent(StringBuilder *sb, const int indent_width, const int depth) {
    TRY(sb_append_char(sb, '\n'));
    const int spaces = indent_width * depth;
    char indent[spaces];
    memset(indent, ' ', spaces);
    const StringView sv = {spaces, indent};
    return sb_append_sv(sb, sv);
}

enum JsonError json_generate_inner(StringBuilder *sb, const JsonValue *value, const int pretty, const int depth) {
    switch (value->type) {
        case JSON_OBJECT:
            TRY(sb_append_char(sb, '{'));
            for (int i = 0; i < value->object.count; ++i) {
                if (pretty > 0) TRY(json_generate_indent(sb, pretty, depth + 1));

                JsonMember member = value->object.members[i];

                TRY(sb_append_char(sb, '"'));
                TRY(sb_append_sv(sb, take_sv_on_string(member.name)));
                TRY(sb_append_char(sb, '"'));
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
            for (int i = 0; i < value->array.count; ++i) {
                if (pretty > 0) TRY(json_generate_indent(sb, pretty, depth + 1));

                JsonValue element = value->array.elements[i];

                TRY(json_generate_inner(sb, &element, pretty, depth + 1));

                // Last in array has no comma
                if (i != value->array.count - 1) TRY(sb_append_char(sb, ','));
            }
            if (pretty > 0 && value->array.count > 0) TRY(json_generate_indent(sb, pretty, depth));

            TRY(sb_append_char(sb, ']'));
            break;
        case JSON_NUMBER:
            if (!isfinite(value->number)) return JSONE_INVALID_NUMBER;

            // Need temporary space because snprintf includes null-terminator
            // DBL_DECIMAL_DIG digits + decimal point + `E+XXX` + null-terminator
            char temp_number_buffer[DBL_DECIMAL_DIG + 1 + 5 + 1];
            sprintf(temp_number_buffer, "%.*G", DBL_DECIMAL_DIG, value->number);
            TRY(sb_append_sv(sb, take_sv_on_c_string(temp_number_buffer)));

            break;
        case JSON_STRING:
            TRY(sb_append_char(sb, '"'));
            TRY(sb_append_sv(sb, take_sv_on_string(value->string)));
            TRY(sb_append_char(sb, '"'));
            break;
        case JSON_BOOLEAN:
            TRY(sb_append_sv(sb, take_sv_on_c_string(value->boolean ? "true" : "false")));
            break;
        case JSON_NULL:
            TRY(sb_append_sv(sb, take_sv_on_c_string("null")));
            break;
    }
    return JSONE_OK;
}

enum JsonError json_generate(StringBuilder *sb, const JsonValue *value, const int pretty_width) {
    return json_generate_inner(sb, value, pretty_width, 0);
}

int main(int argc, char *argv[]) {
    enum JsonError err = JSONE_OK;

    Arena arena = arena_create(MiB(1));
    if (!arena.data) DEFER(JSONE_MEMORY);

    JsonValue root_val = json_object((JsonObject){0});
    JsonObject *root_object = &root_val.object;

    for (int i = 60; i < 70; ++i) {
        char number_as_string[8];
        sprintf(number_as_string, "%d", i);

        json_object_add_member_s(&arena, root_object, number_as_string, json_number(i));
    }

    JsonArray array = {0};
    json_array_append(&arena, &array, json_bool(true));
    json_array_append(&arena, &array, json_bool(false));
    json_array_append(&arena, &array, json_null());
    json_array_append(&arena, &array, json_number(3.141592653589793238462643383279));
    json_array_append(&arena, &array, json_number(3141592653589793238462643383279.141592653589793238462643383279));

    json_array_append(&arena, &array, json_object((JsonObject){0}));

    JsonArray array_of_array = {0};
    const JsonArray empty_array = {0};
    json_array_append(&arena, &array_of_array, json_array(empty_array));
    json_array_append(&arena, &array, json_array(array_of_array));

    String abc;
    copy_c_string_to_string(&arena, "abc", &abc);
    json_array_append(&arena, &array, json_string(abc));

    json_object_add_member_s(&arena, root_object, "array", json_array(array));

    JsonObject query = {0};
    {
        json_object_add_member_s_s(&arena, &query, "TableName", "MeasurementsTable");
        json_object_add_member_s(&arena, &query, "Limit", json_number(1));
        json_object_add_member_s_s(&arena, &query, "KeyConditionExpression", "device_id = :d");

        JsonObject pk_ref = {0};
        {
            JsonObject pk = {0};
            { json_object_add_member_s_s(&arena, &pk, "S", "picotherm/1"); }
            json_object_add_member_s(&arena, &pk_ref, ":d", json_object(pk));
        }
        json_object_add_member_s(&arena, &query, "ExpressionAttributeValues", json_object(pk_ref));

        json_object_add_member_s(&arena, &query, "ScanIndexForward", json_bool(false));
    }

    json_object_add_member_s(&arena, root_object, "Query", json_object(query));

    // Let the StringBuilder allocate its own memory in the arena. Nothing else can alloc on the arena until returned.
    StringBuilder sb = {
        .length = 0,
        .capacity = arena.capacity - arena.used,
        .chars = arena.data + arena.used,
    };

    err = json_generate(&sb, &root_val, 4);
    if (err != JSONE_OK) DEFER(err);

    // Update the arena with the memory the StringBuilder used.
    arena.used += sb.length;

    if (sb.length > 0) {
        printf("%s\n", sb.chars);
    } else {
        printf("Empty string\n");
    }

    const JsonMember *six_seven = json_object_get_s(root_object, "67");
    if (six_seven) {
        printf("%f\n", six_seven->value.number);
    } else {
        printf("67 not found\n");
    }

    {
        const JsonMember *q = json_object_get_s(root_object, "Query");
        const JsonMember *val = json_object_get_s(&q->value.object, "ExpressionAttributeValues");
        const JsonMember *d = json_object_get_s(&val->value.object, ":d");
        const JsonMember *s = json_object_get_s(&d->value.object, "S");
        const StringView sv = take_sv_on_string(s->value.string);
        LOG_INFO("pk: %.*s", (int)sv.length, sv.chars);
    }

defer:
    arena_destroy(&arena);
    return err;
}

// TODO U+0000 is a valid character so we need to use our own string representation that can contain 0
// TODO Add alignment to arena
// TODO String escapes and unicode handling
// Tape??