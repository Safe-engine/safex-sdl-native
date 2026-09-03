#pragma once

#include <quickjs.h>

int js_websocket_client_init(JSContext *ctx);
void js_websocket_client_pump(JSContext *ctx);
void js_websocket_client_shutdown(JSContext *ctx);
