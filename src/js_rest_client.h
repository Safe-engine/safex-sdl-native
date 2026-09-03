#pragma once

#include <quickjs.h>

int js_rest_client_init(JSContext *ctx);
void js_rest_client_pump(JSContext *ctx);
void js_rest_client_shutdown(JSContext *ctx);
