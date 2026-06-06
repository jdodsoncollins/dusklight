#include "touch_controller.hpp"

#include "aurora/lib/logging.hpp"
#include "dusk/ui/ui.hpp"
#include "imgui.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <dolphin/pad.h>
#include <string_view>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace dusk::touch_controller {
namespace {

aurora::Module Log{"dusk::touch_controller"};

constexpr bool kEnabled =
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
    true;
#else
    false;
#endif

constexpr Uint16 kVendorId = 0x5457;   // 'TW'
constexpr Uint16 kProductId = 0x4443;  // 'DC'
constexpr float kButtonAlpha = 0.36f;
constexpr float kPressedAlpha = 0.68f;

enum class Control {
    None,
    LeftStick,
    CStick,
    A,
    B,
    X,
    Y,
    Start,
    Z,
    L,
    R,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
};

struct FingerState {
    SDL_FingerID id = 0;
    Control control = Control::None;
    float x = 0.0f;
    float y = 0.0f;
};

struct CircleControl {
    Control control;
    const char* label;
    float x;
    float y;
    float radius;
};

struct RectControl {
    Control control;
    const char* label;
    float x;
    float y;
    float w;
    float h;
};

SDL_JoystickID sJoystickId = 0;
SDL_Joystick* sJoystick = nullptr;
std::array<FingerState, 12> sFingers{};
bool sInitialized = false;
std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> sButtons{};
std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> sAxes{};

float clamp01(float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

float distance(float ax, float ay, float bx, float by) noexcept {
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

float aspect() noexcept {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.y <= 0.0f) {
        return 1.0f;
    }
    return io.DisplaySize.x / io.DisplaySize.y;
}

float x_radius(float yRadius) noexcept {
    return yRadius / std::max(aspect(), 0.01f);
}

std::array<CircleControl, 11> circle_controls() noexcept {
    return {{
        {Control::A, "A", 0.860f, 0.720f, 0.068f},
        {Control::B, "B", 0.770f, 0.775f, 0.048f},
        {Control::X, "X", 0.925f, 0.655f, 0.045f},
        {Control::Y, "Y", 0.820f, 0.615f, 0.045f},
        {Control::Start, "START", 0.510f, 0.835f, 0.042f},
        {Control::Z, "Z", 0.935f, 0.450f, 0.043f},
        {Control::DpadUp, "", 0.205f, 0.708f, 0.035f},
        {Control::DpadDown, "", 0.205f, 0.816f, 0.035f},
        {Control::DpadLeft, "", 0.168f, 0.762f, 0.035f},
        {Control::DpadRight, "", 0.242f, 0.762f, 0.035f},
        {Control::CStick, "C", 0.690f, 0.820f, 0.070f},
    }};
}

std::array<RectControl, 2> rect_controls() noexcept {
    return {{
        {Control::L, "L", 0.060f, 0.120f, 0.180f, 0.070f},
        {Control::R, "R", 0.760f, 0.120f, 0.180f, 0.070f},
    }};
}

Control hit_test(float x, float y) noexcept {
    if (distance(x, y, 0.185f, 0.610f) <= 0.105f) {
        return Control::LeftStick;
    }
    for (const auto& rect : rect_controls()) {
        if (x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h) {
            return rect.control;
        }
    }
    for (const auto& circle : circle_controls()) {
        const float rx = x_radius(circle.radius);
        const float dx = (x - circle.x) / std::max(rx, 0.001f);
        const float dy = (y - circle.y) / std::max(circle.radius, 0.001f);
        if (dx * dx + dy * dy <= 1.0f) {
            return circle.control;
        }
    }
    return Control::None;
}

FingerState* finger_for_id(SDL_FingerID id) noexcept {
    for (auto& finger : sFingers) {
        if (finger.control != Control::None && finger.id == id) {
            return &finger;
        }
    }
    return nullptr;
}

FingerState* free_finger() noexcept {
    for (auto& finger : sFingers) {
        if (finger.control == Control::None) {
            return &finger;
        }
    }
    return nullptr;
}

bool control_pressed(Control control) noexcept {
    return std::ranges::any_of(sFingers, [control](const FingerState& finger) {
        return finger.control == control;
    });
}

void set_button(SDL_GamepadButton button, bool pressed) noexcept {
    if (button <= SDL_GAMEPAD_BUTTON_INVALID || button >= SDL_GAMEPAD_BUTTON_COUNT) {
        return;
    }
    sButtons[button] = pressed;
    if (sJoystick != nullptr) {
        SDL_SetJoystickVirtualButton(sJoystick, button, pressed);
    }
}

void set_axis(SDL_GamepadAxis axis, Sint16 value) noexcept {
    if (axis <= SDL_GAMEPAD_AXIS_INVALID || axis >= SDL_GAMEPAD_AXIS_COUNT) {
        return;
    }
    sAxes[axis] = value;
    if (sJoystick != nullptr) {
        SDL_SetJoystickVirtualAxis(sJoystick, axis, value);
    }
}

void set_stick_axes(Control control, SDL_GamepadAxis xAxis, SDL_GamepadAxis yAxis,
                   float centerX, float centerY, float radiusY) noexcept {
    float x = 0.0f;
    float y = 0.0f;
    const float radiusX = x_radius(radiusY);
    for (const auto& finger : sFingers) {
        if (finger.control != control) {
            continue;
        }
        x = (finger.x - centerX) / std::max(radiusX, 0.001f);
        y = (finger.y - centerY) / std::max(radiusY, 0.001f);
        const float magnitude = std::sqrt(x * x + y * y);
        if (magnitude > 1.0f) {
            x /= magnitude;
            y /= magnitude;
        }
        break;
    }
    set_axis(xAxis, static_cast<Sint16>(std::clamp(x, -1.0f, 1.0f) * SDL_JOYSTICK_AXIS_MAX));
    set_axis(yAxis, static_cast<Sint16>(std::clamp(y, -1.0f, 1.0f) * SDL_JOYSTICK_AXIS_MAX));
}

void reset_virtual_state() noexcept {
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
        set_button(static_cast<SDL_GamepadButton>(i), false);
    }
    set_axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
    set_axis(SDL_GAMEPAD_AXIS_LEFTY, 0);
    set_axis(SDL_GAMEPAD_AXIS_RIGHTX, 0);
    set_axis(SDL_GAMEPAD_AXIS_RIGHTY, 0);
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    set_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
}

bool has_touch_controller_at_index(u32 index) noexcept {
    const char* name = PADGetNameForControllerIndex(index);
    return name != nullptr && std::string_view{name} == kDeviceName;
}

int touch_controller_index() noexcept {
    const u32 count = PADCount();
    for (u32 index = 0; index < count; ++index) {
        if (has_touch_controller_at_index(index)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

ImVec2 to_screen(float x, float y) noexcept {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    return {x * size.x, y * size.y};
}

ImU32 color(bool pressed, float alphaScale = 1.0f) noexcept {
    const float alpha = (pressed ? kPressedAlpha : kButtonAlpha) * alphaScale;
    return ImGui::GetColorU32(ImVec4(0.96f, 0.92f, 0.68f, alpha));
}

void draw_circle(ImDrawList* drawList, const CircleControl& control, float alphaScale) noexcept {
    const ImVec2 center = to_screen(control.x, control.y);
    const float radius = control.radius * ImGui::GetIO().DisplaySize.y;
    const bool pressed = control_pressed(control.control);
    drawList->AddCircleFilled(center, radius, color(pressed, alphaScale), 48);
    drawList->AddCircle(center, radius, ImGui::GetColorU32(ImVec4(0.98f, 0.90f, 0.35f, 0.85f * alphaScale)), 48, 2.0f);
    if (control.label[0] != '\0') {
        const ImVec2 textSize = ImGui::CalcTextSize(control.label);
        drawList->AddText({center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f},
                          ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.04f, 0.90f * alphaScale)), control.label);
    }
}

void draw_rect(ImDrawList* drawList, const RectControl& control, float alphaScale) noexcept {
    const ImVec2 p0 = to_screen(control.x, control.y);
    const ImVec2 p1 = to_screen(control.x + control.w, control.y + control.h);
    const bool pressed = control_pressed(control.control);
    drawList->AddRectFilled(p0, p1, color(pressed, alphaScale), 18.0f);
    drawList->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.98f, 0.90f, 0.35f, 0.85f * alphaScale)), 18.0f, 0, 2.0f);
    const ImVec2 textSize = ImGui::CalcTextSize(control.label);
    drawList->AddText({(p0.x + p1.x - textSize.x) * 0.5f, (p0.y + p1.y - textSize.y) * 0.5f},
                      ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.04f, 0.90f * alphaScale)), control.label);
}

void draw_stick(ImDrawList* drawList, Control control, const char* label, float x, float y, float radiusY,
                float alphaScale) noexcept {
    const ImVec2 center = to_screen(x, y);
    const float radius = radiusY * ImGui::GetIO().DisplaySize.y;
    ImVec2 knob = center;
    for (const auto& finger : sFingers) {
        if (finger.control != control) {
            continue;
        }
        const float radiusX = x_radius(radiusY);
        float nx = (finger.x - x) / std::max(radiusX, 0.001f);
        float ny = (finger.y - y) / std::max(radiusY, 0.001f);
        const float magnitude = std::sqrt(nx * nx + ny * ny);
        if (magnitude > 1.0f) {
            nx /= magnitude;
            ny /= magnitude;
        }
        knob = to_screen(x + nx * radiusX * 0.70f, y + ny * radiusY * 0.70f);
        break;
    }
    const bool pressed = control_pressed(control);
    drawList->AddCircleFilled(center, radius, color(pressed, alphaScale), 64);
    drawList->AddCircle(center, radius, ImGui::GetColorU32(ImVec4(0.98f, 0.90f, 0.35f, 0.85f * alphaScale)), 64, 2.0f);
    drawList->AddCircleFilled(knob, radius * 0.42f, ImGui::GetColorU32(ImVec4(0.10f, 0.10f, 0.08f, 0.60f * alphaScale)), 36);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    drawList->AddText({center.x - textSize.x * 0.5f, center.y - radius - textSize.y - 4.0f},
                      ImGui::GetColorU32(ImVec4(0.96f, 0.92f, 0.68f, 0.85f * alphaScale)), label);
}

}  // namespace

void initialize() noexcept {
    if constexpr (!kEnabled) {
        return;
    }
    if (sInitialized) {
        return;
    }
    sInitialized = true;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = kVendorId;
    desc.product_id = kProductId;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                       (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                       (1u << SDL_GAMEPAD_BUTTON_START) | (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
                       (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) | (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) |
                       (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) | (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) |
                       (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                     (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
                     (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    desc.name = kDeviceName;

    sJoystickId = SDL_AttachVirtualJoystick(&desc);
    if (sJoystickId == 0) {
        Log.warn("Failed to attach iOS touch controller: {}", SDL_GetError());
        return;
    }
    sJoystick = SDL_OpenJoystick(sJoystickId);
    if (sJoystick == nullptr) {
        Log.warn("Failed to open iOS touch controller: {}", SDL_GetError());
        return;
    }
    reset_virtual_state();
}

void shutdown() noexcept {
    if constexpr (!kEnabled) {
        return;
    }
    reset_virtual_state();
    if (sJoystick != nullptr) {
        SDL_CloseJoystick(sJoystick);
        sJoystick = nullptr;
    }
    if (sJoystickId != 0) {
        SDL_DetachVirtualJoystick(sJoystickId);
        sJoystickId = 0;
    }
    sInitialized = false;
}

bool available() noexcept {
    if constexpr (!kEnabled) {
        return false;
    }
    return sJoystickId != 0;
}

bool assigned() noexcept {
    if (!available()) {
        return false;
    }
    for (int port = PAD_CHAN0; port < PAD_CHANMAX; ++port) {
        const char* name = PADGetName(port);
        if (name != nullptr && std::string_view{name} == kDeviceName) {
            return true;
        }
    }
    return false;
}

void assign_to_port(int port) noexcept {
    if (!available() || port < PAD_CHAN0 || port >= PAD_CHANMAX) {
        return;
    }
    const int index = touch_controller_index();
    if (index >= 0) {
        PADSetKeyboardActive(static_cast<u32>(port), FALSE);
        PADSetPortForIndex(static_cast<u32>(index), static_cast<u32>(port));
    }
}

void handle_event(const SDL_Event& event) noexcept {
    if (!assigned()) {
        return;
    }
    if (event.type != SDL_EVENT_FINGER_DOWN && event.type != SDL_EVENT_FINGER_MOTION &&
        event.type != SDL_EVENT_FINGER_UP && event.type != SDL_EVENT_FINGER_CANCELED)
    {
        return;
    }

    if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
        if (auto* finger = finger_for_id(event.tfinger.fingerID)) {
            *finger = {};
        }
        return;
    }

    const float x = clamp01(event.tfinger.x);
    const float y = clamp01(event.tfinger.y);
    FingerState* finger = finger_for_id(event.tfinger.fingerID);
    if (finger == nullptr && event.type == SDL_EVENT_FINGER_DOWN) {
        const Control control = hit_test(x, y);
        if (control != Control::None) {
            finger = free_finger();
            if (finger != nullptr) {
                finger->id = event.tfinger.fingerID;
                finger->control = control;
            }
        }
    }
    if (finger != nullptr) {
        finger->x = x;
        finger->y = y;
    }
}

void update() noexcept {
    if (!available()) {
        return;
    }
    if (!assigned()) {
        for (auto& finger : sFingers) {
            finger = {};
        }
    }

    set_button(SDL_GAMEPAD_BUTTON_SOUTH, control_pressed(Control::A));
    set_button(SDL_GAMEPAD_BUTTON_EAST, control_pressed(Control::B));
    set_button(SDL_GAMEPAD_BUTTON_WEST, control_pressed(Control::X));
    set_button(SDL_GAMEPAD_BUTTON_NORTH, control_pressed(Control::Y));
    set_button(SDL_GAMEPAD_BUTTON_START, control_pressed(Control::Start));
    set_button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, control_pressed(Control::Z));
    set_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, control_pressed(Control::L));
    set_button(SDL_GAMEPAD_BUTTON_DPAD_UP, control_pressed(Control::DpadUp));
    set_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, control_pressed(Control::DpadDown));
    set_button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, control_pressed(Control::DpadLeft));
    set_button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, control_pressed(Control::DpadRight));

    set_stick_axes(Control::LeftStick, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, 0.185f, 0.610f, 0.105f);
    set_stick_axes(Control::CStick, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, 0.690f, 0.820f, 0.070f);
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
             control_pressed(Control::L) ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    set_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
             control_pressed(Control::R) ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
}

void draw() noexcept {
    if (!assigned() || ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    const float alphaScale = ui::any_document_visible() ? 0.48f : 1.0f;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    draw_stick(drawList, Control::LeftStick, "STICK", 0.185f, 0.610f, 0.105f, alphaScale);
    draw_stick(drawList, Control::CStick, "C", 0.690f, 0.820f, 0.070f, alphaScale);
    for (const auto& rect : rect_controls()) {
        draw_rect(drawList, rect, alphaScale);
    }
    for (const auto& circle : circle_controls()) {
        if (circle.control != Control::CStick) {
            draw_circle(drawList, circle, alphaScale);
        }
    }
}

}  // namespace dusk::touch_controller
