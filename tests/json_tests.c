
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DS_IMPLEMENTATION
#include "../ds.h"
#define JSON_IMPLEMENTATION
#include "../json.h"

#define LOG_DEBUG(fmt, ...) fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)

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
static const char *TEST_JSON_STRING = JSON_STR({"60":60,"61":61,"62":62,"63":63,"64":64,"65":65,"66":66,"67":67,"68":68,"69":69,"array":[true,false,null,3.1415926535897931,-3.1415926535897932E+30,{},[[]],"abc"],"Query":{"TableName":"MeasurementsTable","Limit":1,"KeyConditionExpression":"device_id = :d","ExpressionAttributeValues":{":pk":{"S":"picotherm/1"}},"ScanIndexForward":false},"Unicode character with 2 bytes":"π","Unicode character with 3 bytes":"歌","Unicode character with 4 bytes":"𝄞","Emoji":"⁶🤷‍♂️⁷","Reverse solidus escapes":"\" \\ \b \f \n \r \t","Must escape control characters":"\u001F \u001E \u001D \u001C \u001B \u001A \u0019 \u0018 \u0017 \u0016 \u0015 \u0014 \u0013 \u0012 \u0011 \u0010 \u000F \u000E \r \f \u000B \n \t \b \u0007 \u0006 \u0005 \u0004 \u0003 \u0002 \u0001 \u0000"});
// clang-format on

static void test_json_build_generate() {
    JsonArena arena;
    json_arena_create(&arena, 1024 * 1024, 1024 * 1024);

    JsonValue root_val = json_object((JsonObject){0});
    JsonObject *root_obj = &root_val.object;

    for (int i = 60; i < 70; ++i) {
        char number_as_string[8];
        sprintf(number_as_string, "%d", i);

        json_obj_add(&arena, root_obj, number_as_string, json_number(i));
    }

    JsonArray array = {0};
    json_array_append(&arena.structure, &array, json_bool(true));
    json_array_append(&arena.structure, &array, json_bool(false));
    json_array_append(&arena.structure, &array, json_null());
    json_array_append(&arena.structure, &array, json_number(3.141592653589793238462643383279));
    json_array_append(&arena.structure, &array,
                      json_number(-3141592653589793238462643383279.141592653589793238462643383279));

    json_array_append(&arena.structure, &array, json_object((JsonObject){0}));

    JsonArray array_of_array = {0};
    const JsonArray empty_array = {0};
    json_array_append(&arena.structure, &array_of_array, json_array(empty_array));
    json_array_append(&arena.structure, &array, json_array(array_of_array));

    const StringView abc = cstr_copy_to_arena(&arena.strings, "abc");
    json_array_append(&arena.structure, &array, json_string(abc));

    json_obj_add(&arena, root_obj, "array", json_array(array));

    {
        JsonObject query = {0};
        json_obj_add_string(&arena, &query, "TableName", "MeasurementsTable");
        json_obj_add(&arena, &query, "Limit", json_number(1));
        json_obj_add_string(&arena, &query, "KeyConditionExpression", "device_id = :d");

        {
            JsonObject pk_ref = {0};
            {
                JsonObject pk = {0};
                json_obj_add_string(&arena, &pk, "S", "picotherm/1");
                json_obj_add(&arena, &pk_ref, ":pk", json_object(pk));
            }
            json_obj_add(&arena, &query, "ExpressionAttributeValues", json_object(pk_ref));
        }

        json_obj_add(&arena, &query, "ScanIndexForward", json_bool(false));
        json_obj_add(&arena, root_obj, "Query", json_object(query));
    }

    json_obj_add_string(&arena, root_obj, "Unicode character with 2 bytes", "π");
    json_obj_add_string(&arena, root_obj, "Unicode character with 3 bytes", "歌");
    json_obj_add_string(&arena, root_obj, "Unicode character with 4 bytes", "𝄞");
    json_obj_add_string(&arena, root_obj, "Emoji", "⁶🤷‍♂️⁷");
    json_obj_add_string(&arena, root_obj, "Reverse solidus escapes", "\" \\ \b \f \n \r \t");
    {
        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        for (char i = 31; i >= 0; i--) {
            sb_append_char(&sb, i);
            if (i > 0) sb_append_char(&sb, ' ');
        }

        const StringView sv = sb_complete_and_release_arena(&sb);
        json_obj_add(&arena, root_obj, "Must escape control characters", json_string(sv));
    }

    LOG_INFO("Arena used %zu + %zu = %zu bytes", arena.structure.used, arena.strings.used,
             arena.structure.used + arena.strings.used);

    StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);

    const enum JsonError err = json_generate(&sb, &root_val);
    assert(err == JSON_OK);

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
        assert(sv_equals(sv_on_cstr("picotherm/1"), *device_id));
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
        const enum JsonError err = json_generate_string(&sb, sv_on_cstr((char *)INVALID_UTF_STRINGS[i]));
        sb_complete_and_release_arena(&sb);
        assert(JSONE_GEN_UTF8_INVALID == err);
    }

    arena_destroy(&arena);
}

static void test_json_lexer_simple_tokens() {
    Arena arena;
    const char *text = "[] {}\r:,\ntrue\tfalse null";
    StringView sv = sv_on_cstr(text);
    Token t;

    enum JsonError err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
    assert(t.kind == JSON_KIND_BEGIN_ARRAY);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
    assert(t.kind == JSON_KIND_END_ARRAY);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
    assert(t.kind == JSON_KIND_BEGIN_OBJECT);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
    assert(t.kind == JSON_KIND_END_OBJECT);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
    assert(t.kind == JSON_KIND_NAME_SEPARATOR);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
    assert(t.kind == JSON_KIND_VALUE_SEPARATOR);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
    assert(t.kind == JSON_KIND_BOOLEAN);
    assert(t.boolean == true);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
    assert(t.kind == JSON_KIND_BOOLEAN);
    assert(t.boolean == false);

    err = json_lex_next_token(&arena, &t, &sv);
    assert(err == JSON_OK);
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
        StringView sv = sv_on_cstr(test.string);
        Token t;
        const enum JsonError err = json_lex_next_number(&t, &sv);
        assert(err == JSON_OK);
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
        StringView sv = sv_on_cstr(fail_cases[i]);
        Token t;
        const enum JsonError err = json_lex_next_number(&t, &sv);
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
        {"\"\\u0041\"", "A"},
        {"\"\\u03C0\"", "π"},
        {"\"\\u6B4C\"", "歌"},
        {"\"\\uD834\\uDD1E\"", "𝄞"},
        {"\"\\uD840\\uDC00\"", "𠀀"},
        {"\"\\u0041\\uD834\\uDD1E\\u6B4C\"", "A𝄞歌"},
        // UTF-8 encoding boundaries
        {"\"\\u007F\"", "\x7F"},
        {"\"\\u0080\"", "\xC2\x80"},
        {"\"\\u07FF\"", "\xDF\xBF"},
        {"\"\\u0800\"", "\xE0\xA0\x80"},
        {"\"\\uFFFF\"", "\xEF\xBF\xBF"},
        // Four-byte UTF-8 leading-byte boundaries
        {"\"\\uD800\\uDC00\"", "\xF0\x90\x80\x80"},  // U+10000
        {"\"\\uD8C0\\uDC00\"", "\xF1\x80\x80\x80"},  // U+40000
        {"\"\\uD9C0\\uDC00\"", "\xF2\x80\x80\x80"},  // U+80000
        {"\"\\uDAC0\\uDC00\"", "\xF3\x80\x80\x80"},  // U+C0000
        {"\"\\uDBC0\\uDC00\"", "\xF4\x80\x80\x80"},  // U+100000
        {"\"\\uDBFF\\uDFFF\"", "\xF4\x8F\xBF\xBF"},  // U+10FFFF
    };

    for (int i = 0; i < sizeof(success_cases) / sizeof(*success_cases); ++i) {
        StringView sv = sv_on_cstr(success_cases[i].string);
        Token t;
        const enum JsonError err = json_lex_next_string(&arena, &t, &sv);
        if (err != JSON_OK) {
            LOG_ERROR(SV_FMT ": %s", SV_ARG(sv), error_get_message(err));
            assert(err == JSON_OK);
        }
        assert(t.kind == JSON_KIND_STRING);
        if (!sv_equals(t.string, sv_on_cstr(success_cases[i].expected))) {
            LOG_ERROR("Expected %s but was " SV_FMT, success_cases[i].expected, SV_ARG(t.string));
            assert(0);
        }
    }

    struct TestCaseFail {
        char *string;
        enum JsonError expected;
    };

    const struct TestCaseFail fail_cases[] = {
        {"\"hello", JSONE_PARSE_STRING_UNCLOSED},
        {"\"\\", JSONE_PARSE_BACKLASH_ESCAPE_INVALID},
        {"\"\\x\"", JSONE_PARSE_BACKLASH_ESCAPE_INVALID},
        {"\"\b\"", JSONE_PARSE_CONTROL_CHARACTER_UNESCAPED},
        {"\"\x1\"", JSONE_PARSE_CONTROL_CHARACTER_UNESCAPED},
        {"\"\\u001\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
        {"\"\\ua\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
        {"\"\\u12G4\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
        {"\"\\uD834\"", JSONE_PARSE_HEX_ESCAPE_INVALID},         // isolated high surrogate
        {"\"\\uDD1E\"", JSONE_PARSE_HEX_ESCAPE_INVALID},         // isolated low surrogate
        {"\"\\uDD1E\\uD834\"", JSONE_PARSE_HEX_ESCAPE_INVALID},  // high surrogate after low surrogate
        // high surrogate followed hex escape which is not low surrogate
        {"\"\\uD834\\u0000\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
        // high surrogates followed by invalid escapes
        {"\"\\uD800\\u12G4\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
        {"\"\\uD800x\"", JSONE_PARSE_HEX_ESCAPE_INVALID},
        {"\"\\uD800\\\\n\"", JSONE_PARSE_HEX_ESCAPE_INVALID},

    };

    for (int i = 0; i < sizeof(fail_cases) / sizeof(*fail_cases); ++i) {
        StringView sv = sv_on_cstr(fail_cases[i].string);
        Token t;
        const enum JsonError err = json_lex_next_string(&arena, &t, &sv);
        if (fail_cases[i].expected != err) {
            LOG_ERROR("For string %s expected '%s' but got %s", fail_cases[i].string,
                      error_get_message(fail_cases[i].expected), error_get_message(err));
            assert(0);
        }
    }

    for (int i = 0; i < sizeof(INVALID_UTF_STRINGS) / sizeof(*INVALID_UTF_STRINGS); ++i) {
        StringBuilder sb = sb_create_and_borrow_arena(&arena);
        sb_append_char(&sb, '"');
        sb_append_sv(&sb, sv_on_cstr((char *)INVALID_UTF_STRINGS[i]));
        sb_append_char(&sb, '"');
        sb_append_char(&sb, 0);
        StringView sv = sb_complete_and_release_arena(&sb);
        Token t;
        const enum JsonError err = json_lex_next_string(&arena, &t, &sv);
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
        StringView test_string = sv_on_cstr("true");
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == JSON_OK);
        assert(v->type == JSON_BOOLEAN);
        assert(v->boolean == true);
    }
    {
        StringView test_string = sv_on_cstr("\t null");
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == JSON_OK);
        assert(v->type == JSON_NULL);
    }
    {
        StringView test_string = sv_on_cstr("\n   67  ");
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == JSON_OK);
        assert(v->type == JSON_NUMBER);
        assert(v->number == 67);
    }
    {
        StringView test_string = sv_on_cstr("\"hello \"");
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == JSON_OK);
        assert(v->type == JSON_STRING);
        assert(sv_equals(v->string, sv_on_cstr("hello ")));
    }
    {
        const StringView test_string = sv_on_cstr(JSON_STR([ 1, false, [], [[[[["hello"]]]]] ]));
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == JSON_OK);
        assert(v->type == JSON_ARRAY);

        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        json_generate(&sb, v);
        const StringView output_string = sb_complete_and_release_arena(&sb);
        assert(sv_equals(sv_on_cstr("[1,false,[],[[[[[\"hello\"]]]]]]"), output_string));
    }
    {
        StringView test_string = sv_on_cstr("[ ]");
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == JSON_OK);
        assert(v->type == JSON_ARRAY);

        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        json_generate(&sb, v);
        const StringView output_string = sb_complete_and_release_arena(&sb);
        assert(sv_equals(sv_on_cstr("[]"), output_string));
    }
    {
        StringView test_string = sv_on_cstr(JSON_STR(
            {"hello" : "world", "object" : {"nested" : 1}, "empty" : {}, "more nested" : {"" : {"" : [ {}, 67 ]}}}));
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == JSON_OK);
        assert(v->type == JSON_OBJECT);

        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        json_generate(&sb, v);
        const StringView output_string = sb_complete_and_release_arena(&sb);
        assert(sv_equals(
            sv_on_cstr(
                "{\"hello\":\"world\",\"object\":{\"nested\":1},\"empty\":{},\"more nested\":{\"\":{\"\":[{},67]}}}"),
            output_string));
    }
    {
        StringView test_string = sv_on_cstr(TEST_JSON_STRING);
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        if (err != JSON_OK) {
            assert(error_message.length > 0);
            LOG_ERROR(SV_FMT, SV_ARG(error_message));
            assert(0);
        }

        StringBuilder sb = sb_create_and_borrow_arena(&arena.strings);
        json_generate(&sb, v);
        const StringView output_string = sb_complete_and_release_arena(&sb);
        assert(sv_equals(sv_on_cstr(TEST_JSON_STRING), output_string));
    }
    {
        StringView test_string = sv_on_cstr("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]");
        const enum JsonError err = json_parse(&arena, &v, &error_message, test_string);
        assert(err == JSON_OK);
    }

    json_arena_destroy(&arena);
}

static void test_json_parse_errors() {
    JsonArena arena;
    json_arena_create(&arena, 1024 * 1024, 1024 * 1024);

    JsonValue *v;
    StringView error_message = {0};
    enum JsonError err = 0;

    struct TestCase {
        char *input;
        enum JsonError expected_error;
    };

    const struct TestCase cases[] = {
        {"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]",
         JSONE_PARSE_DEPTH_LIMIT_EXCEEDED},
        {"invalid", JSONE_PARSE_CHARACTER_UNEXPECTED},
        {"[1 2]", JSONE_PARSE_ARRAY_SEPARATOR_MISSING},
        {"[}]", JSONE_PARSE_TOKEN_UNEXPECTED},
        {"[1,2,]", JSONE_PARSE_TOKEN_UNEXPECTED},
        {"[1,2,3[", JSONE_PARSE_ARRAY_SEPARATOR_MISSING},
        {JSON_STR({"a" : 1 "b" : 2}), JSONE_PARSE_OBJECT_SEPARATOR_MISSING},
        {"{]}", JSONE_PARSE_OBJECT_NAME_MISSING},
        {JSON_STR({
             "a" : 1,
             "b" : 2,
         }),
         JSONE_PARSE_TOKEN_UNEXPECTED},
        {JSON_STR({"a" 1}), JSONE_PARSE_NAME_SEPARATOR_MISSING}};

    for (int i = 0; i < sizeof(cases) / sizeof(struct TestCase); ++i) {
        error_message.length = 0;
        const StringView sv = sv_on_cstr(cases[i].input);
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
            for (int i = 0; i < v.object.count; ++i) { sum += count_json_objects(v.object.items[i].value); }
            return sum + 1;
        }
        case JSON_ARRAY: {
            int sum = 0;
            for (int i = 0; i < v.array.count; ++i) { sum += count_json_objects(v.array.items[i]); }
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
        const enum JsonError err =
            json_parse(&arena, &json_value, &error_message, (StringView){file.file_size, file.data});
        if (err != JSON_OK) {
            LOG_ERROR(SV_FMT, SV_ARG(error_message));
            assert(0);
        }
        if (test_runs == 1) {
            LOG_INFO("Arena used %zu + %zu = %zu bytes", arena.structure.used, arena.strings.used,
                     arena.structure.used + arena.strings.used);
        }
        arena_clear(&arena.structure);
        arena_clear(&arena.strings);
    }

    // ReSharper disable once CppDFAConstantConditions
    if (test_runs > 1) {
        // 0.75 ms
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
