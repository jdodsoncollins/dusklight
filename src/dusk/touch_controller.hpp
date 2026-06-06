#pragma once

#include <SDL3/SDL_events.h>

namespace dusk::touch_controller {

constexpr const char* kDeviceName = "Touch Controller";

void initialize() noexcept;
void shutdown() noexcept;
void handle_event(const SDL_Event& event) noexcept;
void update() noexcept;
void draw() noexcept;
bool available() noexcept;
bool assigned() noexcept;
void assign_to_port(int port) noexcept;

}  // namespace dusk::touch_controller
