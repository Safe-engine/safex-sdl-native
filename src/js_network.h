#pragma once

#include <quickjs.h>

int js_network_init(JSContext *ctx);
void js_network_pump(JSContext *ctx);
void js_network_shutdown(JSContext *ctx);
