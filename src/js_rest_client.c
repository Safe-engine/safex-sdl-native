#include "js_rest_client.h"

#include <curl/curl.h>
#include <SDL3/SDL.h>
#include <string.h>

typedef struct HttpRequest {
    char *url;
    char *method;
    struct curl_slist *headers;
    uint8_t *request_body;
    size_t request_body_len;
    uint8_t *response_body;
    size_t response_body_len;
    size_t response_body_capacity;
    CURL *easy;
    CURLcode result;
    JSValue resolve;
    JSValue reject;
    struct HttpRequest *next;
} HttpRequest;

typedef struct RestApiClient {
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    SDL_Condition *condition;
    bool stopping;
    HttpRequest *pending_head;
    HttpRequest *pending_tail;
    HttpRequest *completed_head;
    HttpRequest *completed_tail;
} RestApiClient;

static RestApiClient g_rest_client;

static JSValue resolved_promise(JSContext *ctx, JSValue value)
{
    JSValue functions[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, value);
        return promise;
    }
    JSValue ignored = JS_Call(ctx, functions[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, ignored);
    JS_FreeValue(ctx, functions[0]);
    JS_FreeValue(ctx, functions[1]);
    JS_FreeValue(ctx, value);
    return promise;
}

static void free_http_request(JSContext *ctx, HttpRequest *request)
{
    if (request->easy) curl_easy_cleanup(request->easy);
    curl_slist_free_all(request->headers);
    SDL_free(request->url);
    SDL_free(request->method);
    SDL_free(request->request_body);
    SDL_free(request->response_body);
    JS_FreeValue(ctx, request->resolve);
    JS_FreeValue(ctx, request->reject);
    SDL_free(request);
}

static size_t http_write_callback(char *data, size_t size, size_t count, void *userdata)
{
    HttpRequest *request = userdata;
    size_t length = size * count;
    if (length > SIZE_MAX - request->response_body_len) return 0;
    size_t required = request->response_body_len + length;
    if (required > request->response_body_capacity) {
        size_t capacity = request->response_body_capacity ? request->response_body_capacity : 4096;
        while (capacity < required) capacity *= 2;
        uint8_t *body = SDL_realloc(request->response_body, capacity);
        if (!body) return 0;
        request->response_body = body;
        request->response_body_capacity = capacity;
    }
    SDL_memcpy(request->response_body + request->response_body_len, data, length);
    request->response_body_len += length;
    return length;
}

static JSValue response_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JSValue body = JS_GetPropertyStr(ctx, this_val, "_body");
    size_t length = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &length, body);
    JSValue text = bytes ? JS_NewStringLen(ctx, (const char *)bytes, length) : JS_EXCEPTION;
    JS_FreeValue(ctx, body);
    return resolved_promise(ctx, text);
}

static JSValue response_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JSValue body = JS_GetPropertyStr(ctx, this_val, "_body");
    size_t length = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &length, body);
    JSValue value = bytes ? JS_ParseJSON(ctx, (const char *)bytes, length, "fetch response") : JS_EXCEPTION;
    JS_FreeValue(ctx, body);
    return JS_IsException(value) ? value : resolved_promise(ctx, value);
}

static JSValue response_array_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JSValue body = JS_GetPropertyStr(ctx, this_val, "_body");
    size_t length = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &length, body);
    JSValue copy = bytes ? JS_NewArrayBufferCopy(ctx, bytes, length) : JS_EXCEPTION;
    JS_FreeValue(ctx, body);
    return resolved_promise(ctx, copy);
}

static JSValue new_response(JSContext *ctx, HttpRequest *request, long status, const char *url)
{
    JSValue response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, (int)status));
    JS_SetPropertyStr(ctx, response, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, response, "url", JS_NewString(ctx, url ? url : ""));
    JS_SetPropertyStr(ctx, response, "headers", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, response, "_body", JS_NewArrayBufferCopy(ctx, request->response_body, request->response_body_len));
    JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, response_text, "text", 0));
    JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, response_json, "json", 0));
    JS_SetPropertyStr(ctx, response, "arrayBuffer", JS_NewCFunction(ctx, response_array_buffer, "arrayBuffer", 0));
    return response;
}

static void reject_request(JSContext *ctx, HttpRequest *request, const char *message)
{
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message));
    JSValue ignored = JS_Call(ctx, request->reject, JS_UNDEFINED, 1, &error);
    JS_FreeValue(ctx, ignored);
    JS_FreeValue(ctx, error);
}

static bool add_request_headers(JSContext *ctx, HttpRequest *request, JSValueConst headers)
{
    if (!JS_IsObject(headers)) return true;
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &properties, &count, headers, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        const char *key = JS_AtomToCString(ctx, properties[i].atom);
        JSValue value = JS_GetProperty(ctx, headers, properties[i].atom);
        const char *text = JS_ToCString(ctx, value);
        char *header = NULL;
        if (key && text) SDL_asprintf(&header, "%s: %s", key, text);
        if (header) {
            request->headers = curl_slist_append(request->headers, header);
            SDL_free(header);
        }
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, value);
        JS_FreeAtom(ctx, properties[i].atom);
    }
    js_free(ctx, properties);
    return true;
}

static void enqueue_completed(HttpRequest *request)
{
    SDL_LockMutex(g_rest_client.mutex);
    request->next = NULL;
    if (g_rest_client.completed_tail) g_rest_client.completed_tail->next = request;
    else g_rest_client.completed_head = request;
    g_rest_client.completed_tail = request;
    SDL_UnlockMutex(g_rest_client.mutex);
}

static void configure_request(HttpRequest *request)
{
    request->easy = curl_easy_init();
    if (!request->easy) {
        request->result = CURLE_FAILED_INIT;
        return;
    }
    curl_easy_setopt(request->easy, CURLOPT_URL, request->url);
    curl_easy_setopt(request->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request->easy, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, request);
    curl_easy_setopt(request->easy, CURLOPT_PRIVATE, request);
    if (request->method) curl_easy_setopt(request->easy, CURLOPT_CUSTOMREQUEST, request->method);
    if (request->headers) curl_easy_setopt(request->easy, CURLOPT_HTTPHEADER, request->headers);
    if (request->request_body) {
        curl_easy_setopt(request->easy, CURLOPT_POSTFIELDS, request->request_body);
        curl_easy_setopt(request->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->request_body_len);
    }
}

static int rest_api_worker(void *userdata)
{
    (void)userdata;
    CURLM *multi = curl_multi_init();
    HttpRequest *active = NULL;
    while (multi) {
        SDL_LockMutex(g_rest_client.mutex);
        HttpRequest *pending = g_rest_client.pending_head;
        g_rest_client.pending_head = NULL;
        g_rest_client.pending_tail = NULL;
        bool stopping = g_rest_client.stopping;
        SDL_UnlockMutex(g_rest_client.mutex);

        while (pending) {
            HttpRequest *request = pending;
            pending = pending->next;
            configure_request(request);
            if (!request->easy) {
                enqueue_completed(request);
                continue;
            }
            curl_multi_add_handle(multi, request->easy);
            request->next = active;
            active = request;
        }

        if (stopping) {
            while (active) {
                HttpRequest *request = active;
                active = request->next;
                curl_multi_remove_handle(multi, request->easy);
                request->result = CURLE_ABORTED_BY_CALLBACK;
                enqueue_completed(request);
            }
            break;
        }

        int running = 0;
        curl_multi_perform(multi, &running);
        int messages = 0;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &messages))) {
            if (message->msg != CURLMSG_DONE) continue;
            HttpRequest *request = NULL;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
            if (!request) continue;
            request->result = message->data.result;
            curl_multi_remove_handle(multi, request->easy);
            HttpRequest **link = &active;
            while (*link && *link != request) link = &(*link)->next;
            if (*link) *link = request->next;
            enqueue_completed(request);
        }

        if (active) {
            SDL_Delay(10);
        } else {
            SDL_LockMutex(g_rest_client.mutex);
            if (!g_rest_client.stopping && !g_rest_client.pending_head)
                SDL_WaitConditionTimeout(g_rest_client.condition, g_rest_client.mutex, 100);
            SDL_UnlockMutex(g_rest_client.mutex);
        }
    }
    curl_multi_cleanup(multi);
    return 0;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fetch requires a URL");
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    HttpRequest *request = SDL_calloc(1, sizeof(*request));
    JSValue functions[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    if (!request || JS_IsException(promise)) {
        SDL_free(request);
        JS_FreeCString(ctx, url);
        JS_FreeValue(ctx, functions[0]);
        JS_FreeValue(ctx, functions[1]);
        return JS_EXCEPTION;
    }
    request->resolve = functions[0];
    request->reject = functions[1];
    request->url = SDL_strdup(url);
    JS_FreeCString(ctx, url);
    if (!request->url) {
        free_http_request(ctx, request);
        JS_FreeValue(ctx, promise);
        return JS_ThrowOutOfMemory(ctx);
    }

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue method = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(method)) {
            const char *method_text = JS_ToCString(ctx, method);
            if (method_text) request->method = SDL_strdup(method_text);
            JS_FreeCString(ctx, method_text);
        }
        JS_FreeValue(ctx, method);
        JSValue headers = JS_GetPropertyStr(ctx, argv[1], "headers");
        bool valid_headers = add_request_headers(ctx, request, headers);
        JS_FreeValue(ctx, headers);
        if (!valid_headers) {
            free_http_request(ctx, request);
            JS_FreeValue(ctx, promise);
            return JS_EXCEPTION;
        }
        JSValue body = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
            size_t length = 0;
            uint8_t *bytes = JS_GetArrayBuffer(ctx, &length, body);
            const char *text = bytes ? NULL : JS_ToCStringLen(ctx, &length, body);
            if (bytes || text) {
                request->request_body = SDL_malloc(length);
                if (request->request_body) SDL_memcpy(request->request_body, bytes ? bytes : (const uint8_t *)text, length);
                request->request_body_len = length;
            }
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, body);
    }

    SDL_LockMutex(g_rest_client.mutex);
    if (g_rest_client.stopping) {
        SDL_UnlockMutex(g_rest_client.mutex);
        reject_request(ctx, request, "REST API client is shutting down");
        free_http_request(ctx, request);
        return promise;
    }
    if (g_rest_client.pending_tail) g_rest_client.pending_tail->next = request;
    else g_rest_client.pending_head = request;
    g_rest_client.pending_tail = request;
    SDL_SignalCondition(g_rest_client.condition);
    SDL_UnlockMutex(g_rest_client.mutex);
    return promise;
}

int js_rest_client_init(JSContext *ctx)
{
    g_rest_client.mutex = SDL_CreateMutex();
    g_rest_client.condition = SDL_CreateCondition();
    if (!g_rest_client.mutex || !g_rest_client.condition) {
        if (g_rest_client.condition) SDL_DestroyCondition(g_rest_client.condition);
        if (g_rest_client.mutex) SDL_DestroyMutex(g_rest_client.mutex);
        SDL_zero(g_rest_client);
        return -1;
    }
    g_rest_client.thread = SDL_CreateThread(rest_api_worker, "rest-api", NULL);
    if (!g_rest_client.thread) {
        SDL_DestroyCondition(g_rest_client.condition);
        SDL_DestroyMutex(g_rest_client.mutex);
        SDL_zero(g_rest_client);
        return -1;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_FreeValue(ctx, global);
    return 0;
}

void js_rest_client_pump(JSContext *ctx)
{
    SDL_LockMutex(g_rest_client.mutex);
    HttpRequest *request = g_rest_client.completed_head;
    g_rest_client.completed_head = NULL;
    g_rest_client.completed_tail = NULL;
    SDL_UnlockMutex(g_rest_client.mutex);
    while (request) {
        HttpRequest *next = request->next;
        if (request->result == CURLE_OK) {
            long status = 0;
            char *url = NULL;
            curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_getinfo(request->easy, CURLINFO_EFFECTIVE_URL, &url);
            JSValue response = new_response(ctx, request, status, url);
            JSValue ignored = JS_Call(ctx, request->resolve, JS_UNDEFINED, 1, &response);
            JS_FreeValue(ctx, ignored);
            JS_FreeValue(ctx, response);
        } else {
            reject_request(ctx, request, curl_easy_strerror(request->result));
        }
        free_http_request(ctx, request);
        request = next;
    }
}

void js_rest_client_shutdown(JSContext *ctx)
{
    if (!g_rest_client.mutex) return;
    SDL_LockMutex(g_rest_client.mutex);
    g_rest_client.stopping = true;
    SDL_BroadcastCondition(g_rest_client.condition);
    SDL_UnlockMutex(g_rest_client.mutex);
    if (g_rest_client.thread) SDL_WaitThread(g_rest_client.thread, NULL);
    js_rest_client_pump(ctx);
    SDL_DestroyCondition(g_rest_client.condition);
    SDL_DestroyMutex(g_rest_client.mutex);
    SDL_zero(g_rest_client);
}
