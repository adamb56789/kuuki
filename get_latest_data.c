#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The type-checking on curl_easy_setopt and similar expands to huge macros which times out the analyzer,
// messes up the syntax highlighting, and somehow started randomly deleting and moving lines of code?
#ifdef __CLION_IDE__
#define CURL_DISABLE_TYPECHECK
#endif
#include <curl/curl.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define LATENCY_LOG_ENABLE
// #define CURL_DEBUG

#define STR(...) #__VA_ARGS__

#define LOG_DEBUG(fmt, ...) fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)

#define FATAL_ERROR(fmt, ...)                                                                                          \
    do {                                                                                                               \
        LOG_ERROR(fmt, ##__VA_ARGS__);                                                                                 \
        exit(1);                                                                                                       \
    } while (0)

static const char *REQUEST_ID_HEADER_NAME = "Lambda-Runtime-Aws-Request-Id";
static const char *INVOCATION_ID_HEADER_NAME = "Lambda-Runtime-Invocation-Id";

static const char *CONTENT_TYPE_JSON_HEADER = "Content-Type: application/x-amz-json-1.0";

#define AWS_REGION "eu-west-1"
static const char *DYNAMODB_ENDPOINT = "https://dynamodb." AWS_REGION ".amazonaws.com";

#define X_AMZ_TARGET_PREFIX "X-Amz-Target: "
#define X_AMZ_TARGET_DYNAMODB X_AMZ_TARGET_PREFIX "DynamoDB_20120810."
static const char *DYNAMODB_QUERY = X_AMZ_TARGET_DYNAMODB "Query";

static const char *ACCESS_DENIED_RESPONSE = STR({"statusCode" : 403, "body" : "Access denied"});

// TODO encode in HTML <link rel="icon" href="data:image/x-icon;base64,AAABAAEAEBAAAAAAAABoBQAAF/QA6OjoAKysrABwcH"/>
static const char *FAVICON = STR({
    "statusCode" : 200,
    "body" : "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"12\" height=\"12\">"
             "<text x=\"0\" y=\"10\" font-size=\"12\">気</text>"
             "</svg>",
    "headers" : {"content-type" : "image/svg+xml"}
});

static const char *get_env_or_fatal(const char *name) {
    const char *value = getenv(name);
    if (!value)
        FATAL_ERROR("Missing env var: %s", name);
    return value;
}

typedef struct {
    const char *access_key_id;
    const char *secret_access_key;
    const char *session_token;
    const char *api_key;
} Env;

static Env load_env_vars() {
    return (Env) {
            get_env_or_fatal("AWS_ACCESS_KEY_ID"),
            get_env_or_fatal("AWS_SECRET_ACCESS_KEY"),
            get_env_or_fatal("AWS_SESSION_TOKEN"),
            get_env_or_fatal("API_KEY"),
    };
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} CurlResponse;

typedef struct {
    CURL *handle;
    CurlResponse *response_data;
    char *userpwd; // AWS_ACCESS_KEY_ID:AWS_SECRET_ACCESS_KEY
    char *session_token_header; // x-amz-security-token: AWS_SESSION_TOKEN
} HttpClient;

static size_t curl_write_fixed_buffer_callback(const char *ptr, const size_t size, const size_t nmemb, void *userdata) {
    CurlResponse *resp = userdata;

    const size_t bytes = size * nmemb;

    const size_t new_length = resp->length + bytes;
    if (new_length > resp->capacity) {
        LOG_ERROR("Response size %zu exceeded buffer capacity of %zu", new_length, resp->capacity);
        // Fill the buffer with what we can to aid with debugging
        memcpy(resp->data + resp->length, ptr, resp->capacity - resp->length);
        resp->length = resp->capacity;
        return CURL_WRITEFUNC_ERROR;
    }

    memcpy(resp->data + resp->length, ptr, bytes);
    resp->length += bytes;
    return bytes;
}

#ifdef CURL_DEBUG
static int curl_print_debug_callback(CURL *handle, curl_infotype type, char *data, const size_t size, void *userdata) {
    int print_size = (int) size;
    if (size > 0 && data[size - 1] == '\n')
        print_size--;
    LOG_DEBUG("CURL: %.*s", (int) print_size, data);
    return 0;
}
#endif

void curl_set_default_options(const HttpClient *client) {
    curl_easy_reset(client->handle);

    curl_easy_setopt(client->handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(client->handle, CURLOPT_CONNECTTIMEOUT, 1L);

    curl_easy_setopt(client->handle, CURLOPT_WRITEDATA, client->response_data);
    curl_easy_setopt(client->handle, CURLOPT_WRITEFUNCTION, curl_write_fixed_buffer_callback);

    client->response_data->length = 0; // Reset response buffer

#ifdef CURL_DEBUG
    curl_easy_setopt(client->handle, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(client->handle, CURLOPT_DEBUGFUNCTION, curl_print_debug_callback);
#endif
}

void curl_set_aws_options(const HttpClient *client, struct curl_slist *headers) {
    curl_set_default_options(client);

    curl_easy_setopt(client->handle, CURLOPT_AWS_SIGV4, "aws:amz");
    curl_easy_setopt(client->handle, CURLOPT_USERPWD, client->userpwd);

    headers = curl_slist_append(headers, client->session_token_header);
    curl_easy_setopt(client->handle, CURLOPT_HTTPHEADER, headers);
}

// The memory for the returned struct is associated with the easy handle and subsequent calls to curl_easy_header
// clobber the struct used in the previous calls for the same easy handle.
char *get_response_header(const HttpClient *client, const char *name) {
    struct curl_header *header;
    const CURLHcode code = curl_easy_header(client->handle, name, 0, CURLH_HEADER, -1, &header);

    if (code != CURLHE_OK) {
        FATAL_ERROR("Failed to get %s header with code %d", name, code);
    }
    return header->value;
}

HttpClient init_http_client(const Env *env, const size_t response_data_buffer_size) {
    CURL *handle = curl_easy_init();

    CurlResponse *curl_response = malloc(sizeof(*curl_response));
    *curl_response = (CurlResponse) {malloc(response_data_buffer_size + 1), 0, response_data_buffer_size};

    char *userpwd;
    asprintf(&userpwd, "%s:%s", env->access_key_id, env->secret_access_key);

    char *session_token_header = NULL;
    asprintf(&session_token_header, "x-amz-security-token: %s", env->session_token);
    return (HttpClient) {handle, curl_response, userpwd, session_token_header};
}

int perform_http(const HttpClient *client, struct curl_slist *headers, const char *name_for_log) {
    const CURLcode curl_code = curl_easy_perform(client->handle);

#ifdef LATENCY_LOG_ENABLE
    long total_time;
    curl_easy_getinfo(client->handle, CURLINFO_TOTAL_TIME_T, &total_time);

    LOG_INFO("%s latency: %lu", name_for_log, total_time);
#endif

    curl_slist_free_all(headers);

    if (curl_code != CURLE_OK) {
        LOG_ERROR("curl failed with code: %d - %s", curl_code, curl_easy_strerror(curl_code));
        return 999;
    }
    long http_code;
    curl_easy_getinfo(client->handle, CURLINFO_RESPONSE_CODE, &http_code);
    return (int) http_code;
}

int perform_dynamodb(const HttpClient *client, const char *operation, const char *name_for_log, char *input) {
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, CONTENT_TYPE_JSON_HEADER);
    headers = curl_slist_append(headers, operation);

    curl_set_aws_options(client, headers);

    curl_easy_setopt(client->handle, CURLOPT_URL, DYNAMODB_ENDPOINT);
    curl_easy_setopt(client->handle, CURLOPT_POSTFIELDS, input);

    return perform_http(client, headers, name_for_log);
}

typedef struct {
    float humidity;
    float temperature;
    u64 timestamp_ms;
} Measurement;

int query(const HttpClient *client, Measurement *measurement) {
    char *input = STR({
        "TableName" : "MeasurementsTable",
        "Limit" : 1,
        "KeyConditionExpression" : "device_id = :d",
        "ExpressionAttributeValues" : {":d" : {"S" : "picotherm/1"}},
        "ScanIndexForward" : false
    });

    const int code = perform_dynamodb(client, DYNAMODB_QUERY, "Test Query", input);
    const CurlResponse response = *client->response_data;
    if (code > 200) {
        LOG_ERROR("Query returned code %d: %.*s", code, (int) response.length, response.data);
        return 1;
    }

    char *humidity = strstr(response.data, "humidity");
    char *temperature = strstr(response.data, "temperature");
    char *timestamp_str = strstr(response.data, "time");
    if (!humidity || !temperature || !timestamp_str) {
        LOG_ERROR("Found no data for device_id: picotherm/1");
        return 1;
    }

    measurement->humidity = strtof(humidity + 16, NULL);
    measurement->temperature = strtof(temperature + 19, NULL);
    measurement->timestamp_ms = strtol(timestamp_str + 12, NULL, 10);
    return 0;
}

int main(int argc, char *argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const Env env = load_env_vars();

    const HttpClient client = init_http_client(&env, 10000);

    char *handler = getenv("_HANDLER");
    if (!handler) {
        Measurement measurement;
        query(&client, &measurement);

        LOG_INFO("%lu %.1f %.1f", measurement.timestamp_ms, measurement.humidity, measurement.temperature);
        return 0;
    }

    const char *lambda_runtime_endpoint = get_env_or_fatal("AWS_LAMBDA_RUNTIME_API");
    char *next_api_url;
    asprintf(&next_api_url, "http://%s/2018-06-01/runtime/invocation/next", lambda_runtime_endpoint);

    while (1) {
        curl_set_default_options(&client);

        curl_easy_setopt(client.handle, CURLOPT_URL, next_api_url);

        LOG_INFO("GET from %s", next_api_url);
        const int next_api_code = perform_http(&client, NULL, "");

        if (next_api_code >= 400) {
            FATAL_ERROR("Failed to get next invocation with code %d", next_api_code);
        }

        char request_id[64];
        strcpy(request_id, get_response_header(&client, REQUEST_ID_HEADER_NAME));

        char invocation_id[64];
        strcpy(invocation_id, get_response_header(&client, INVOCATION_ID_HEADER_NAME));

        char *event_data = client.response_data->data;
        LOG_INFO("Received event: %.*s", (int) client.response_data->length, event_data);

        char response_data[256];

        event_data[client.response_data->length] = 0; // Add null-terminator for strstr
        if (strstr(event_data, "/favicon.ico")) {
            strcpy(response_data, FAVICON);
        } else if (!strstr(event_data, env.api_key)) {
            strcpy(response_data, ACCESS_DENIED_RESPONSE);
        } else {
            Measurement measurement;
            query(&client, &measurement);

            snprintf(response_data, sizeof(response_data), STR({"humidity" : "%.1f", "temperature" : "%.1f"}),
                     measurement.humidity, measurement.temperature);
        }

        curl_set_default_options(&client);
        curl_easy_setopt(client.handle, CURLOPT_POSTFIELDS, response_data);

        char *response_url;
        asprintf(&response_url, "http://%s/2018-06-01/runtime/invocation/%s/response", lambda_runtime_endpoint,
                 request_id);
        curl_easy_setopt(client.handle, CURLOPT_URL, response_url);

        char invocation_id_header[100];
        sprintf(invocation_id_header, "%s: %s", INVOCATION_ID_HEADER_NAME, invocation_id);
        curl_easy_setopt(client.handle, CURLOPT_HTTPHEADER, curl_slist_append(NULL, invocation_id_header));

        LOG_INFO("POST to %s. Response data: %s", response_url, response_data);
        const int response_api_code = perform_http(&client, NULL, "");

        if (response_api_code >= 400) {
            FATAL_ERROR("Failed to post response with code %d: %.*s", response_api_code,
                        (int) client.response_data->length, client.response_data->data);
        }
    }
}
