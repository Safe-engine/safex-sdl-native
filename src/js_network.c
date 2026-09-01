#include "js_network.h"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <SDL3/SDL.h>
#include <string.h>

typedef struct HttpRequest {
    CURL *easy;
    struct curl_slist *headers;
    uint8_t *request_body;
    size_t request_body_len;
    uint8_t *response_body;
    size_t response_body_len;
    size_t response_body_capacity;
    JSValue resolve;
    JSValue reject;
    struct HttpRequest *next;
} HttpRequest;

typedef enum WsEventType {
    WS_EVENT_OPEN,
    WS_EVENT_MESSAGE,
    WS_EVENT_ERROR,
    WS_EVENT_CLOSE,
} WsEventType;

typedef struct WsEvent {
    WsEventType type;
    char *message;
    uint8_t *data;
    size_t data_len;
    bool binary;
    struct WsEvent *next;
} WsEvent;

typedef struct WebSocketState {
    JSValue object;
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    char *url;
    uint8_t *pending_send;
    size_t pending_send_len;
    bool pending_send_binary;
    bool closing;
    bool done;
    WsEvent *events_head;
    WsEvent *events_tail;
    struct WebSocketState *next;
} WebSocketState;

static CURLM *g_http_multi;
static HttpRequest *g_http_requests;
static WebSocketState *g_websockets;
static JSClassID g_websocket_class_id;

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
    request->easy = curl_easy_init();
    if (!request->easy) {
        reject_request(ctx, request, "Unable to create HTTP client");
        free_http_request(ctx, request);
        JS_FreeCString(ctx, url);
        return promise;
    }

    curl_easy_setopt(request->easy, CURLOPT_URL, url);
    curl_easy_setopt(request->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request->easy, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, request);
    curl_easy_setopt(request->easy, CURLOPT_PRIVATE, request);

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue method = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(method)) {
            const char *method_text = JS_ToCString(ctx, method);
            if (method_text) curl_easy_setopt(request->easy, CURLOPT_CUSTOMREQUEST, method_text);
            JS_FreeCString(ctx, method_text);
        }
        JS_FreeValue(ctx, method);

        JSValue headers = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (!add_request_headers(ctx, request, headers)) {
            JS_FreeValue(ctx, headers);
            free_http_request(ctx, request);
            JS_FreeCString(ctx, url);
            JS_FreeValue(ctx, promise);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, headers);

        JSValue body = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
            size_t length = 0;
            uint8_t *bytes = JS_GetArrayBuffer(ctx, &length, body);
            const char *text = bytes ? NULL : JS_ToCStringLen(ctx, &length, body);
            if (bytes || text) {
                request->request_body = SDL_malloc(length);
                if (request->request_body) SDL_memcpy(request->request_body, bytes ? bytes : (const uint8_t *)text, length);
                request->request_body_len = length;
                curl_easy_setopt(request->easy, CURLOPT_POSTFIELDS, request->request_body);
                curl_easy_setopt(request->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)length);
            }
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, body);
    }
    if (request->headers) curl_easy_setopt(request->easy, CURLOPT_HTTPHEADER, request->headers);
    curl_multi_add_handle(g_http_multi, request->easy);
    request->next = g_http_requests;
    g_http_requests = request;
    JS_FreeCString(ctx, url);
    return promise;
}

static void ws_push_event(WebSocketState *socket, WsEventType type, const char *message, const uint8_t *data, size_t data_len, bool binary)
{
    WsEvent *event = SDL_calloc(1, sizeof(*event));
    if (!event) return;
    event->type = type;
    event->binary = binary;
    event->data_len = data_len;
    if (message) event->message = SDL_strdup(message);
    if (data_len) {
        event->data = SDL_malloc(data_len);
        if (event->data) SDL_memcpy(event->data, data, data_len);
    }
    SDL_LockMutex(socket->mutex);
    if (socket->events_tail) socket->events_tail->next = event;
    else socket->events_head = event;
    socket->events_tail = event;
    SDL_UnlockMutex(socket->mutex);
}

static int websocket_worker(void *userdata)
{
    WebSocketState *socket = userdata;
    CURL *curl = curl_easy_init();
    if (!curl) {
        ws_push_event(socket, WS_EVENT_ERROR, "Unable to create WebSocket client", NULL, 0, false);
        ws_push_event(socket, WS_EVENT_CLOSE, NULL, NULL, 0, false);
        socket->done = true;
        return 0;
    }
    curl_easy_setopt(curl, CURLOPT_URL, socket->url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        ws_push_event(socket, WS_EVENT_ERROR, curl_easy_strerror(result), NULL, 0, false);
        ws_push_event(socket, WS_EVENT_CLOSE, NULL, NULL, 0, false);
        curl_easy_cleanup(curl);
        socket->done = true;
        return 0;
    }
    ws_push_event(socket, WS_EVENT_OPEN, NULL, NULL, 0, false);
    uint8_t buffer[16384];
    while (true) {
        SDL_LockMutex(socket->mutex);
        bool closing = socket->closing;
        uint8_t *outgoing = socket->pending_send;
        size_t outgoing_len = socket->pending_send_len;
        bool outgoing_binary = socket->pending_send_binary;
        socket->pending_send = NULL;
        socket->pending_send_len = 0;
        SDL_UnlockMutex(socket->mutex);
        if (closing) break;
        if (outgoing) {
            size_t sent = 0;
            result = curl_ws_send(curl, outgoing, outgoing_len, &sent, 0, outgoing_binary ? CURLWS_BINARY : CURLWS_TEXT);
            SDL_free(outgoing);
            if (result != CURLE_OK && result != CURLE_AGAIN) {
                ws_push_event(socket, WS_EVENT_ERROR, curl_easy_strerror(result), NULL, 0, false);
                break;
            }
        }
        size_t received = 0;
        const struct curl_ws_frame *meta = NULL;
        result = curl_ws_recv(curl, buffer, sizeof(buffer), &received, &meta);
        if (result == CURLE_OK && received) {
            bool binary = meta && (meta->flags & CURLWS_BINARY);
            ws_push_event(socket, WS_EVENT_MESSAGE, NULL, buffer, received, binary);
        } else if (result != CURLE_AGAIN && result != CURLE_OK) {
            ws_push_event(socket, WS_EVENT_ERROR, curl_easy_strerror(result), NULL, 0, false);
            break;
        }
        SDL_Delay(10);
    }
    curl_easy_cleanup(curl);
    ws_push_event(socket, WS_EVENT_CLOSE, NULL, NULL, 0, false);
    socket->done = true;
    return 0;
}

static void websocket_finalizer(JSRuntime *runtime, JSValue value)
{
    (void)runtime;
    WebSocketState *socket = JS_GetOpaque(value, g_websocket_class_id);
    if (socket && socket->mutex) {
        SDL_LockMutex(socket->mutex);
        socket->closing = true;
        SDL_UnlockMutex(socket->mutex);
    }
}

static JSValue js_websocket_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WebSocketState *socket = JS_GetOpaque2(ctx, this_val, g_websocket_class_id);
    if (!socket) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "WebSocket.send requires data");
    size_t length = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &length, argv[0]);
    const char *text = bytes ? NULL : JS_ToCStringLen(ctx, &length, argv[0]);
    if (!bytes && !text) return JS_EXCEPTION;
    uint8_t *copy = SDL_malloc(length);
    if (copy) SDL_memcpy(copy, bytes ? bytes : (const uint8_t *)text, length);
    JS_FreeCString(ctx, text);
    if (!copy && length) return JS_ThrowOutOfMemory(ctx);
    SDL_LockMutex(socket->mutex);
    SDL_free(socket->pending_send);
    socket->pending_send = copy;
    socket->pending_send_len = length;
    socket->pending_send_binary = bytes != NULL;
    SDL_UnlockMutex(socket->mutex);
    return JS_UNDEFINED;
}

static JSValue js_websocket_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    WebSocketState *socket = JS_GetOpaque(this_val, g_websocket_class_id);
    if (socket) {
        SDL_LockMutex(socket->mutex);
        socket->closing = true;
        SDL_UnlockMutex(socket->mutex);
    }
    return JS_UNDEFINED;
}

static JSValue js_websocket_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "WebSocket requires a URL");
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    WebSocketState *socket = SDL_calloc(1, sizeof(*socket));
    if (!socket) {
        JS_FreeCString(ctx, url);
        return JS_ThrowOutOfMemory(ctx);
    }
    socket->url = SDL_strdup(url);
    socket->mutex = SDL_CreateMutex();
    JS_FreeCString(ctx, url);
    if (!socket->url || !socket->mutex) {
        SDL_free(socket->url);
        if (socket->mutex) SDL_DestroyMutex(socket->mutex);
        SDL_free(socket);
        return JS_ThrowInternalError(ctx, "Unable to create WebSocket client");
    }
    JSValue object = JS_NewObjectClass(ctx, g_websocket_class_id);
    JS_SetOpaque(object, socket);
    socket->object = JS_DupValue(ctx, object);
    JS_SetPropertyStr(ctx, object, "CONNECTING", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, object, "OPEN", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, object, "CLOSING", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, object, "CLOSED", JS_NewInt32(ctx, 3));
    JS_SetPropertyStr(ctx, object, "readyState", JS_NewInt32(ctx, 0));
    socket->thread = SDL_CreateThread(websocket_worker, "websocket", socket);
    if (!socket->thread) {
        JS_FreeValue(ctx, socket->object);
        SDL_DestroyMutex(socket->mutex);
        SDL_free(socket->url);
        SDL_free(socket);
        return JS_ThrowInternalError(ctx, "Unable to start WebSocket client");
    }
    socket->next = g_websockets;
    g_websockets = socket;
    (void)new_target;
    return object;
}

static void ws_dispatch(JSContext *ctx, WebSocketState *socket, const char *name, JSValue event)
{
    JSValue handler = JS_GetPropertyStr(ctx, socket->object, name);
    if (JS_IsFunction(ctx, handler)) {
        JSValue result = JS_Call(ctx, handler, socket->object, 1, &event);
        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(ctx);
            JS_FreeValue(ctx, exception);
        }
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, handler);
    JS_FreeValue(ctx, event);
}

static void ws_pump_events(JSContext *ctx, WebSocketState *socket)
{
    SDL_LockMutex(socket->mutex);
    WsEvent *event = socket->events_head;
    socket->events_head = NULL;
    socket->events_tail = NULL;
    SDL_UnlockMutex(socket->mutex);
    while (event) {
        WsEvent *next = event->next;
        JSValue js_event = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, js_event, "target", JS_DupValue(ctx, socket->object));
        if (event->type == WS_EVENT_OPEN) {
            JS_SetPropertyStr(ctx, socket->object, "readyState", JS_NewInt32(ctx, 1));
            ws_dispatch(ctx, socket, "onopen", js_event);
        } else if (event->type == WS_EVENT_MESSAGE) {
            JSValue data = event->binary
                ? JS_NewArrayBufferCopy(ctx, event->data, event->data_len)
                : JS_NewStringLen(ctx, (const char *)event->data, event->data_len);
            JS_SetPropertyStr(ctx, js_event, "data", data);
            ws_dispatch(ctx, socket, "onmessage", js_event);
        } else if (event->type == WS_EVENT_ERROR) {
            JS_SetPropertyStr(ctx, js_event, "message", JS_NewString(ctx, event->message ? event->message : "WebSocket error"));
            ws_dispatch(ctx, socket, "onerror", js_event);
        } else {
            JS_SetPropertyStr(ctx, socket->object, "readyState", JS_NewInt32(ctx, 3));
            ws_dispatch(ctx, socket, "onclose", js_event);
        }
        SDL_free(event->message);
        SDL_free(event->data);
        SDL_free(event);
        event = next;
    }
}

int js_network_init(JSContext *ctx)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return -1;
    g_http_multi = curl_multi_init();
    if (!g_http_multi) return -1;
    JS_NewClassID(JS_GetRuntime(ctx), &g_websocket_class_id);
    JSClassDef class_def = { .class_name = "WebSocket", .finalizer = websocket_finalizer };
    JS_NewClass(JS_GetRuntime(ctx), g_websocket_class_id, &class_def);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JSValue websocket = JS_NewCFunction2(ctx, js_websocket_constructor, "WebSocket", 1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, websocket, "CONNECTING", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, websocket, "OPEN", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, websocket, "CLOSING", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, websocket, "CLOSED", JS_NewInt32(ctx, 3));
    JSValue prototype = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, prototype, "send", JS_NewCFunction(ctx, js_websocket_send, "send", 1));
    JS_SetPropertyStr(ctx, prototype, "close", JS_NewCFunction(ctx, js_websocket_close, "close", 0));
    JS_SetClassProto(ctx, g_websocket_class_id, JS_DupValue(ctx, prototype));
    JS_SetPropertyStr(ctx, websocket, "prototype", prototype);
    JS_SetPropertyStr(ctx, global, "WebSocket", websocket);
    JS_FreeValue(ctx, global);
    return 0;
}

void js_network_pump(JSContext *ctx)
{
    if (g_http_multi) {
        int running = 0;
        curl_multi_perform(g_http_multi, &running);
        int messages = 0;
        CURLMsg *message;
        while ((message = curl_multi_info_read(g_http_multi, &messages))) {
            if (message->msg != CURLMSG_DONE) continue;
            HttpRequest *request = NULL;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
            if (!request) continue;
            curl_multi_remove_handle(g_http_multi, request->easy);
            HttpRequest **link = &g_http_requests;
            while (*link && *link != request) link = &(*link)->next;
            if (*link) *link = request->next;
            if (message->data.result == CURLE_OK) {
                long status = 0;
                char *url = NULL;
                curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE, &status);
                curl_easy_getinfo(request->easy, CURLINFO_EFFECTIVE_URL, &url);
                JSValue response = new_response(ctx, request, status, url);
                JSValue ignored = JS_Call(ctx, request->resolve, JS_UNDEFINED, 1, &response);
                JS_FreeValue(ctx, ignored);
                JS_FreeValue(ctx, response);
            } else {
                reject_request(ctx, request, curl_easy_strerror(message->data.result));
            }
            free_http_request(ctx, request);
        }
    }
    for (WebSocketState *socket = g_websockets; socket; socket = socket->next)
        ws_pump_events(ctx, socket);
}

void js_network_shutdown(JSContext *ctx)
{
    while (g_http_requests) {
        HttpRequest *request = g_http_requests;
        g_http_requests = request->next;
        curl_multi_remove_handle(g_http_multi, request->easy);
        free_http_request(ctx, request);
    }
    if (g_http_multi) {
        curl_multi_cleanup(g_http_multi);
        g_http_multi = NULL;
    }
    while (g_websockets) {
        WebSocketState *socket = g_websockets;
        g_websockets = socket->next;
        SDL_LockMutex(socket->mutex);
        socket->closing = true;
        SDL_UnlockMutex(socket->mutex);
        if (socket->thread) SDL_WaitThread(socket->thread, NULL);
        WsEvent *event = socket->events_head;
        while (event) {
            WsEvent *next = event->next;
            SDL_free(event->message);
            SDL_free(event->data);
            SDL_free(event);
            event = next;
        }
        JS_FreeValue(ctx, socket->object);
        SDL_free(socket->pending_send);
        SDL_DestroyMutex(socket->mutex);
        SDL_free(socket->url);
        SDL_free(socket);
    }
    curl_global_cleanup();
}
