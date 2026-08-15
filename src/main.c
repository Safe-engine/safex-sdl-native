#define SDL_MAIN_USE_CALLBACKS 1

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "engine.h"

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    return engine_init(appstate, argc, argv);
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    return engine_handle_event(appstate, event);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    return engine_iterate(appstate);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    engine_quit(appstate, result);
}
