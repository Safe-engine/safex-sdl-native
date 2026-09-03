#include "js_websocket_client.h"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <SDL3/SDL.h>

typedef enum WsEventType { WS_EVENT_OPEN, WS_EVENT_MESSAGE, WS_EVENT_ERROR, WS_EVENT_CLOSE } WsEventType;

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
    WsEvent *events_head;
    WsEvent *events_tail;
    struct WebSocketState *next;
} WebSocketState;

static WebSocketState *g_websockets;
static JSClassID g_websocket_class_id;

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
    return 0;
}

static void websocket_finalizer(JSRuntime *runtime, JSValue value)
{
    (void)runtime;
    WebSocketState *socket = JS_GetOpaque(value, g_websocket_class_id);
    if (!socket || !socket->mutex) return;
    SDL_LockMutex(socket->mutex);
    socket->closing = true;
    SDL_UnlockMutex(socket->mutex);
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
    (void)ctx; (void)argc; (void)argv;
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
    if (!socket) { JS_FreeCString(ctx, url); return JS_ThrowOutOfMemory(ctx); }
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
        if (JS_IsException(result)) { JSValue exception = JS_GetException(ctx); JS_FreeValue(ctx, exception); }
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
            JSValue data = event->binary ? JS_NewArrayBufferCopy(ctx, event->data, event->data_len) : JS_NewStringLen(ctx, (const char *)event->data, event->data_len);
            JS_SetPropertyStr(ctx, js_event, "data", data);
            ws_dispatch(ctx, socket, "onmessage", js_event);
        } else if (event->type == WS_EVENT_ERROR) {
            JS_SetPropertyStr(ctx, js_event, "message", JS_NewString(ctx, event->message ? event->message : "WebSocket error"));
            ws_dispatch(ctx, socket, "onerror", js_event);
        } else {
            JS_SetPropertyStr(ctx, socket->object, "readyState", JS_NewInt32(ctx, 3));
            ws_dispatch(ctx, socket, "onclose", js_event);
        }
        SDL_free(event->message); SDL_free(event->data); SDL_free(event);
        event = next;
    }
}

int js_websocket_client_init(JSContext *ctx)
{
    JS_NewClassID(JS_GetRuntime(ctx), &g_websocket_class_id);
    JSClassDef class_def = { .class_name = "WebSocket", .finalizer = websocket_finalizer };
    JS_NewClass(JS_GetRuntime(ctx), g_websocket_class_id, &class_def);
    JSValue global = JS_GetGlobalObject(ctx);
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

void js_websocket_client_pump(JSContext *ctx)
{
    for (WebSocketState *socket = g_websockets; socket; socket = socket->next) ws_pump_events(ctx, socket);
}

void js_websocket_client_shutdown(JSContext *ctx)
{
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
            SDL_free(event->message); SDL_free(event->data); SDL_free(event);
            event = next;
        }
        JS_FreeValue(ctx, socket->object);
        SDL_free(socket->pending_send);
        SDL_DestroyMutex(socket->mutex);
        SDL_free(socket->url);
        SDL_free(socket);
    }
}
