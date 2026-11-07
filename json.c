#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KiB(n) (n * 1024)
#define MiB(n) (KiB(n) * 1024)
#define GiB(n) (MiB(n) * 1024)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define defer_error(code) \
    do {                  \
        err = code;       \
        goto defer;       \
    } while (0)

typedef enum { JSONE_OK = 0, JSONE_MEMORY } JsonError;

enum JsonType { JSON_OBJECT, JSON_ARRAY, JSON_NUMBER, JSON_STRING, JSON_BOOLEAN, JSON_NULL };

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
        JsonObject *object;
        JsonArray *array;
        char *string;
        double number;
        bool boolean;
    };
};

struct JsonMember {
    const char *name;
    JsonValue value;
};

typedef struct {
    void *data;
    size_t used;
    size_t capacity;
} JsonArena;

typedef struct {
    JsonArena arena;
    JsonValue *root;
} JsonDocument;

JsonDocument json_document_init(const size_t capacity) {
    void *data = malloc(capacity);

    if (!data) return (JsonDocument){0};

    const JsonArena arena = {data, 0, capacity};
    return (JsonDocument){arena, NULL};
}

void *arena_alloc(JsonArena *arena, const size_t size) {
    if (arena->used + size > arena->capacity) return NULL;

    void *ptr = arena->data + arena->used;
    arena->used += size;

    memset(ptr, 0, size);
    return ptr;
}

void *arena_realloc(JsonArena *arena, const void *old_ptr, const size_t old_size, const size_t new_size) {
    assert(new_size > old_size);
    if (arena->used + new_size > arena->capacity) return NULL;

    void *new_ptr = arena->data + arena->used;
    arena->used += new_size;

    memcpy(new_ptr, old_ptr, old_size);
    memset(new_ptr + old_size, 0, new_size - old_size);
    return new_ptr;
}

void *arena_add_char(JsonArena *arena, const char c) {
    if (arena->used >= arena->capacity) return NULL;
    *(char *)&arena->data[arena->used] = c;
    return arena->data + arena->used++;
}

void *arena_add_string(JsonArena *arena, const char *s) {
    const size_t size = strlen(s);
    if (arena->used + size > arena->capacity) return NULL;

    char *name_dest = arena->data + arena->used;
    arena->used += size;

    memcpy(name_dest, s, size);
    return name_dest;
}

JsonError json_object_add_member(JsonArena *arena, JsonObject *object, const JsonMember member) {
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

    object->members[object->count++] = member;
    return JSONE_OK;
}

JsonError json_array_append(JsonArena *arena, JsonArray *array, const JsonValue value) {
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

JsonMember *json_object_get(const JsonObject *object, const char *name) {
    for (size_t i = 0; i < object->count; ++i) {
        if (strcmp(object->members[i].name, name) == 0) {
            return &object->members[i];
        }
    }
    return NULL;
}

JsonError json_generate_inner(JsonArena *arena, const JsonValue *value) {
    switch (value->type) {
        case JSON_OBJECT:
            if (!arena_add_char(arena, '{')) return JSONE_MEMORY;
            for (int i = 0; i < value->object->count; ++i) {
                JsonMember member = value->object->members[i];

                if (!arena_add_char(arena, '"')) return JSONE_MEMORY;

                arena_add_string(arena, member.name);

                if (!arena_add_char(arena, '"')) return JSONE_MEMORY;
                if (!arena_add_char(arena, ':')) return JSONE_MEMORY;

                const JsonError err = json_generate_inner(arena, &member.value);
                if (err != JSONE_OK) return err;

                // Last in object has no comma
                if (i != value->object->count - 1) {
                    if (!arena_add_char(arena, ',')) return JSONE_MEMORY;
                }
            }
            if (!arena_add_char(arena, '}')) return JSONE_MEMORY;
            break;
        case JSON_ARRAY:
            if (!arena_add_char(arena, '[')) return JSONE_MEMORY;
            for (int i = 0; i < value->array->count; ++i) {
                JsonValue element = value->array->elements[i];

                const JsonError err = json_generate_inner(arena, &element);
                if (err != JSONE_OK) return err;

                // Last in array has no comma
                if (i != value->array->count - 1) {
                    if (!arena_add_char(arena, ',')) return JSONE_MEMORY;
                }
            }
            if (!arena_add_char(arena, ']')) return JSONE_MEMORY;
            break;
        case JSON_NUMBER: {
            const int number_length = snprintf(NULL, 0, "%.1f", value->number);
            char *number_dest = arena_alloc(arena, number_length);
            snprintf(number_dest, number_length + 1, "%.1f", value->number);
            break;
        }
        case JSON_STRING:
            if (!arena_add_char(arena, '"')) return JSONE_MEMORY;
            if (!arena_add_string(arena, value->string)) return JSONE_MEMORY;
            if (!arena_add_char(arena, '"')) return JSONE_MEMORY;
            break;
        case JSON_BOOLEAN:
            if (!arena_add_string(arena, value->boolean ? "true" : "false")) return JSONE_MEMORY;
            break;
        case JSON_NULL:
            if (!arena_add_string(arena, "null")) return JSONE_MEMORY;
            break;
    }
    return JSONE_OK;
}

JsonError json_generate(JsonArena *arena, const JsonValue *value) {
    const JsonError err = json_generate_inner(arena, value);
    if (err != JSONE_OK) return err;
    if (!arena_add_char(arena, 0)) return JSONE_MEMORY;
    return JSONE_OK;
}

int main(int argc, char *argv[]) {
    JsonError err = JSONE_OK;

    JsonDocument document = json_document_init(MiB(1));
    JsonArena arena = document.arena;
    if (!arena.data) defer_error(JSONE_MEMORY);

    JsonObject *object = arena_alloc(&arena, sizeof(JsonObject));
    if (!object) defer_error(JSONE_MEMORY);

    document.root = arena_alloc(&arena, sizeof(JsonValue));
    document.root->type = JSON_OBJECT;
    document.root->object = object;

    for (int i = 0; i < 70; ++i) {
        char *name = arena_alloc(&arena, 8);
        sprintf(name, "%d", i);
        const JsonValue value = {JSON_NUMBER, .number = i};
        json_object_add_member(&arena, object, (JsonMember){name, value});
    }

    const JsonMember *six_seven = json_object_get(object, "67");

    JsonArray *array = arena_alloc(&arena, sizeof(JsonArray));
    json_array_append(&arena, array, (JsonValue){JSON_BOOLEAN, .boolean = true});
    json_array_append(&arena, array, (JsonValue){JSON_BOOLEAN, .boolean = false});
    json_array_append(&arena, array, (JsonValue){JSON_NULL});

    JsonObject *empty_object = arena_alloc(&arena, sizeof(JsonObject));
    json_array_append(&arena, array, (JsonValue){JSON_OBJECT, .object = empty_object});

    JsonObject *empty_array = arena_alloc(&arena, sizeof(JsonArray));
    json_array_append(&arena, array, (JsonValue){JSON_ARRAY, .object = empty_array});

    json_object_add_member(&arena, object, (JsonMember){"array", (JsonValue){JSON_ARRAY, .array = array}});

    char *s = arena_add_char(&arena, 'a');
    arena_add_char(&arena, 'b');
    arena_add_char(&arena, 'c');
    arena_add_char(&arena, 0);
    json_array_append(&arena, array, (JsonValue){JSON_STRING, .string = s});

    printf("%f\n", six_seven->value.number);

    const char *json_string = arena.data + arena.used;
    json_generate_inner(&arena, document.root);

    printf("%s\n", json_string);

defer:
    free(document.arena.data);
    return err;
}
// TODO U+0000 is a valid character so we need to use our own string representation that can contain 0
// Tape??