#include "js_network.h"

#include <curl/curl.h>

#include "js_rest_client.h"
#include "js_websocket_client.h"

int js_network_init(JSContext *ctx)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return -1;
    if (js_rest_client_init(ctx) < 0) {
        curl_global_cleanup();
        return -1;
    }
    if (js_websocket_client_init(ctx) < 0) {
        js_rest_client_shutdown(ctx);
        curl_global_cleanup();
        return -1;
    }
    return 0;
}

void js_network_pump(JSContext *ctx)
{
    js_rest_client_pump(ctx);
    js_websocket_client_pump(ctx);
}

void js_network_shutdown(JSContext *ctx)
{
    js_websocket_client_shutdown(ctx);
    js_rest_client_shutdown(ctx);
    curl_global_cleanup();
}
