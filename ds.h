#ifndef DS_H
#define DS_H

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Arena {
    void *data;
    size_t alignment;
    size_t used;
    size_t capacity;
} Arena;

Arena arena_create(size_t capacity, size_t alignment);
void arena_destroy(Arena *arena);
void arena_clear(Arena *arena);
void *arena_alloc(Arena *arena, size_t size);
void *arena_realloc(Arena *arena, void *old_ptr, size_t old_size, size_t new_size);

#define DEFINE_DA_TYPE(array_type, item_type, function_name) \
    typedef struct array_type {                              \
        item_type *items;                                    \
        size_t count;                                        \
        size_t capacity;                                     \
    } array_type;                                            \
                                                             \
    array_type *function_name(Arena *arena, array_type *da, const item_type value)

#define IMPLEMENT_DA_APPEND(function_name, array_type, item_type)                                                    \
    array_type *function_name(Arena *arena, array_type *da, const item_type value) {                                 \
        if (da->count >= da->capacity) {                                                                             \
            const size_t new_capacity = da->capacity == 0 ? 1 : da->capacity * 2;                                    \
                                                                                                                     \
            void *new_elements =                                                                                     \
                arena_realloc(arena, da->items, da->capacity * sizeof(item_type), new_capacity * sizeof(item_type)); \
            if (!new_elements) return NULL;                                                                          \
            da->items = new_elements;                                                                                \
            da->capacity = new_capacity;                                                                             \
        }                                                                                                            \
                                                                                                                     \
        da->items[da->count++] = value;                                                                              \
        return da;                                                                                                   \
    }

typedef struct StringView {
    size_t length;
    char *chars;
} StringView;

typedef struct StringBuilder {
    Arena *arena;
    size_t length;
    size_t capacity;
    char *chars;
} StringBuilder;

#define SV_FMT "%.*s"
#define SV_ARG(sv) (int)sv.length, sv.chars

StringView sv_on_cstr(const char *cstr);
StringView sv_slice(StringView sv, size_t start, size_t end);
StringView sv_copy_to_arena(Arena *arena, StringView sv);
StringView cstr_copy_to_arena(Arena *string_arena, const char *cstr);
StringView sv_create_formatted(Arena *string_arena, const char *format, ...) __attribute__((format(printf, 2, 3)));
void sv_chop_left(StringView *sv, size_t n);
bool sv_equals(StringView sv1, StringView sv2);
// Transfer ownership of the remaining memory in arena to the StringBuilder
// Nothing else can alloc on the arena until the SB is released as the arena spare capacity is shrunk to zero
StringBuilder sb_create_and_borrow_arena(Arena *arena);
// Updates the arena with the memory the StringBuilder used to transfer ownership bac
StringView sb_complete_and_release_arena(StringBuilder *sb);
StringBuilder *sb_append_char(StringBuilder *sb, char c);
StringBuilder *sb_append_sv(StringBuilder *sb, StringView sv);
StringBuilder *sb_append_formatted(StringBuilder *sb, const char *format, ...) __attribute__((format(printf, 2, 3)));

#ifdef DS_IMPLEMENTATION

static size_t align_up(const size_t x, const size_t alignment) { return x + (alignment - 1) & ~(alignment - 1); }

Arena arena_create(const size_t capacity, const size_t alignment) {
    assert(__builtin_popcountll(alignment) == 1);
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

void *arena_realloc(Arena *arena, void *old_ptr, const size_t old_size, const size_t new_size) {
    if (new_size <= old_size) return old_ptr;

    // Arena has not grown so we can expand the existing allocation
    if (arena->data + arena->used == old_ptr + old_size) {
        arena->used += new_size - old_size;
        return old_ptr;
    }

    const size_t offset = align_up(arena->used, arena->alignment);

    if (offset > arena->capacity || new_size > arena->capacity - offset) return NULL;

    void *new_ptr = arena->data + offset;
    arena->used = offset + new_size;

    memcpy(new_ptr, old_ptr, old_size);
    return new_ptr;
}

StringView sv_on_cstr(const char *cstr) { return (StringView){strlen(cstr), (char *)cstr}; }

StringView sv_slice(const StringView sv, size_t start, size_t end) {
    if (start > sv.length) start = sv.length;
    if (end > sv.length) end = sv.length;
    if (end < start) end = start;
    return (StringView){end - start, sv.chars + start};
}

StringView sv_copy_to_arena(Arena *arena, const StringView sv) {
    StringView dest = {0};
    dest.chars = arena_alloc(arena, sv.length);
    if (!dest.chars) return dest;
    memcpy(dest.chars, sv.chars, sv.length);
    dest.length = sv.length;
    return dest;
}

StringView cstr_copy_to_arena(Arena *string_arena, const char *cstr) {
    return sv_copy_to_arena(string_arena, sv_on_cstr(cstr));
}

StringView sv_create_formatted(Arena *string_arena, const char *format, ...) {
    StringView dest = {0};

    va_list args, args_copy;
    va_start(args, format);

    va_copy(args_copy, args);
    const int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    dest.chars = arena_alloc(string_arena, len + 1);
    if (!dest.chars) {
        va_end(args);
        return dest;
    }

    vsnprintf(dest.chars, len + 1, format, args);
    va_end(args);

    dest.length = len;
    return dest;
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

StringView sb_complete_and_release_arena(StringBuilder *sb) {
    // The sb_ functions should check bounds and never allow this
    assert(sb->length <= sb->capacity);

    const StringView sv = {sb->length, sb->chars};

    sb->arena->capacity += sb->capacity;
    sb->arena->used += sb->length;
    sb->capacity = 0;
    sb->length = 0;
    return sv;
}

StringBuilder *sb_append_char(StringBuilder *sb, const char c) {
    if (sb->length >= sb->capacity) return NULL;
    sb->chars[sb->length++] = c;
    return sb;
}

StringBuilder *sb_append_sv(StringBuilder *sb, const StringView sv) {
    if (sb->length + sv.length > sb->capacity) return NULL;
    memcpy(sb->chars + sb->length, sv.chars, sv.length);
    sb->length += sv.length;
    return sb;
}

StringBuilder *sb_append_formatted(StringBuilder *sb, const char *format, ...) {
    va_list args, args_copy;
    va_start(args, format);

    va_copy(args_copy, args);
    const int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    // We need space for the null-terminator produced by printf
    if (sb->length + len + 1 > sb->capacity) return NULL;

    vsnprintf(sb->chars + sb->length, len + 1, format, args);
    va_end(args);

    // Don't include the null-terminator in the resulting string
    sb->length += len;
    return sb;
}

#endif  // DS_IMPLEMENTATION
#endif  // DS_H