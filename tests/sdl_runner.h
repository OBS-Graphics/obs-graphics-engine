// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Minimal SDL3 + Cairo scaffolding for engine test apps.
// Cairo ARGB32 and SDL ARGB8888 have the same pixel layout on little-endian,
// so SDL_UpdateTexture can copy directly from cairo_image_surface_get_data.

#pragma once

#include <SDL3/SDL.h>
#include <cairo/cairo.h>
#include <stdexcept>

struct SDLRunner {
    int width, height;
    SDL_Window*      window{};
    SDL_Renderer*    renderer{};
    SDL_Texture*     texture{};
    cairo_surface_t* surface{};
    cairo_t*         cr{};

    SDLRunner(int w, int h, const char* title) : width(w), height(h)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
            throw std::runtime_error(SDL_GetError());

        window = SDL_CreateWindow(title, w, h, 0);
        if (!window) throw std::runtime_error(SDL_GetError());

        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) throw std::runtime_error(SDL_GetError());
        SDL_SetRenderVSync(renderer, 1);

        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!texture) throw std::runtime_error(SDL_GetError());

        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cr      = cairo_create(surface);
    }

    ~SDLRunner()
    {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }

    // Blit the Cairo surface to the SDL window.
    void flush()
    {
        cairo_surface_flush(surface);
        SDL_UpdateTexture(texture, nullptr,
                          cairo_image_surface_get_data(surface),
                          cairo_image_surface_get_stride(surface));
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    // Event loop. tick(cr, dt_seconds) is called once per frame.
    // Exits when the user closes the window or presses Escape.
    template<typename Tick>
    void run(Tick tick)
    {
        Uint64 prev = SDL_GetTicks();
        for (;;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) return;
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) return;
            }
            Uint64 now = SDL_GetTicks();
            float dt = static_cast<float>(now - prev) / 1000.0f;
            if (dt > 0.1f) dt = 0.1f;  // cap to avoid large jumps after sleep/pause
            prev = now;
            tick(cr, dt);
            flush();
        }
    }
};
