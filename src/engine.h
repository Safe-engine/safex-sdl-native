#pragma once

#include <SDL3/SDL.h>
#include <quickjs.h>

typedef void (*engine_js_bridge_fn)(JSContext *ctx);

SDL_AppResult engine_init(void **appstate, int argc, char *argv[]);
SDL_AppResult engine_handle_event(void *appstate, SDL_Event *event);
SDL_AppResult engine_iterate(void *appstate);
void engine_quit(void *appstate, SDL_AppResult result);
void engine_set_js_bridge_callbacks(
    engine_js_bridge_fn pump,
    engine_js_bridge_fn shutdown);
