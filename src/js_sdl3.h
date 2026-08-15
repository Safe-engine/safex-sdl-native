#pragma once

#include <quickjs.h>
#include <SDL3/SDL.h>

typedef void (*js_main_thread_fn)(void *arg);
void js_run_on_main_thread(js_main_thread_fn fn, void *arg);
bool js_is_main_thread(void);

int  js_init_sdl3(JSContext *ctx);
void js_sdl3_shutdown(JSContext *ctx);
void js_execute_pending_job(JSRuntime *rt);
void js_set_frame_timing(float delta_time);
bool js_enable_render_queue(void);
void js_disable_render_queue(void);
void js_destroy_render_queue(void);
void js_collect_retired_textures(void);
bool js_render_pending_frame(void);

/* helpers for the engine game loop */
void js_call_onInit(JSContext *ctx);
void js_call_onUpdate(JSContext *ctx);
void js_call_onUpdate_dt(JSContext *ctx, float dt);
void js_call_onRender(JSContext *ctx);
void js_call_touchStart(JSContext *ctx, float x, float y);
void js_call_touchMove(JSContext *ctx, float x, float y);
void js_call_touchEnd(JSContext *ctx, float x, float y);
void js_call_textInput(JSContext *ctx, const char *text);
void js_call_keyDown(JSContext *ctx, const char *key);
void js_call_keyUp(JSContext *ctx, const char *key);
void js_call_pause(JSContext *ctx);
void js_call_resume(JSContext *ctx);
void js_call_background(JSContext *ctx);
void js_call_foreground(JSContext *ctx);
void js_call_interruption(JSContext *ctx, int active);
void js_call_low_memory(JSContext *ctx);
void js_call_orientation_change(
    JSContext *ctx, SDL_DisplayOrientation orientation, int width, int height);
void js_call_terminate(JSContext *ctx);
void js_get_window_size(int *width, int *height);
int  js_get_win_w(void);
int  js_get_win_h(void);
void js_convert_event_to_render_coordinates(SDL_Event *event);

typedef struct FrameMetrics {
    Uint64 logic_ns;
    Uint64 js_update_ns;
    Uint64 serialize_ns;
    Uint64 wait_render_buffer_ns;
    Uint64 render_execute_ns;
    Uint64 present_ns;
    Uint64 frame_interval_ns;

    uint32_t command_count;
    uint32_t buffer_grow_count;
    uint32_t dropped_render_frames;
} FrameMetrics;

#if JS_SDL_ENABLE_PROFILING
void js_prof_record_logic(Uint64 logic_ns, Uint64 js_update_ns, Uint64 serialize_ns, Uint64 wait_ns);
void js_prof_record_render(Uint64 exec_ns, Uint64 present_ns, uint32_t cmd_count, Uint64 interval_ns);
void js_prof_record_buffer_grow(void);
void js_prof_record_dropped_frame(void);
#endif
