#pragma once

#include <quickjs.h>

/* Hai binding của module sdl3. */
JSValue js_callNative(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_onNativeResult(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

/* Giao các kết quả đang xếp hàng lên JS. Gọi mỗi frame, trên thread JS. */
void js_native_bridge_pump(JSContext *ctx);

/* Thả callback và hàng đợi lúc tắt máy. */
void js_native_bridge_shutdown(JSContext *ctx);
