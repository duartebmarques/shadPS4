// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <numbers>
#include <thread>
#include <unordered_set>
#include <utility>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/system/userservice.h"
#include "core/prompt_packs/prompt_packs.h"
#include "core/user_settings.h"
#include "input/controller.h"

namespace Input {

using Libraries::Pad::OrbisPadButtonDataOffset;

namespace {

void CalculateOrientation(const Libraries::Pad::OrbisFVector3& angular_velocity, float delta_time,
                          const Libraries::Pad::OrbisFQuaternion& last_orientation,
                          Libraries::Pad::OrbisFQuaternion& orientation) {
    // The first poll and frame stalls produce a huge step, and a first-order Euler step then
    // dwarfs q so the attitude snaps to the angular-velocity direction. Clamp the step instead.
    constexpr float kMaxDeltaTime = 0.05f;
    if (!(delta_time > 0.0f)) {
        orientation = last_orientation;
        return;
    }
    delta_time = std::min(delta_time, kMaxDeltaTime);
    Libraries::Pad::OrbisFQuaternion q = last_orientation;
    const Libraries::Pad::OrbisFQuaternion omega = {angular_velocity.x, angular_velocity.y,
                                                    angular_velocity.z, 0.0f};

    const Libraries::Pad::OrbisFQuaternion q_omega = {
        q.w * omega.x + q.x * omega.w + q.y * omega.z - q.z * omega.y,
        q.w * omega.y + q.y * omega.w + q.z * omega.x - q.x * omega.z,
        q.w * omega.z + q.z * omega.w + q.x * omega.y - q.y * omega.x,
        q.w * omega.w - q.x * omega.x - q.y * omega.y - q.z * omega.z};

    const Libraries::Pad::OrbisFQuaternion q_dot = {0.5f * q_omega.x, 0.5f * q_omega.y,
                                                    0.5f * q_omega.z, 0.5f * q_omega.w};

    q.x += q_dot.x * delta_time;
    q.y += q_dot.y * delta_time;
    q.z += q_dot.z * delta_time;
    q.w += q_dot.w * delta_time;

    const float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= norm;
    q.y /= norm;
    q.z /= norm;
    q.w /= norm;
    orientation = q;
}

} // namespace

GameController::GameController() : m_states_queue(64) {}

State GameController::ReadState() {
    std::lock_guard lock{m_state_mutex};
    return m_state;
}

int GameController::ReadStates(State* states, int states_num) {
    std::lock_guard lock{m_state_mutex};
    if (states_num <= 0) {
        return 0;
    }

    if (!m_state.connected) {
        states[0] = m_state;
        return 1;
    }

    if (states_num == 1) {
        // Retained history can make a later multi-sample read return up to 64 stale reports, so
        // mixed single- and multi-sample reads require dedicated tests.
        states[0] = m_state;
        return 1;
    }

    int read_count = 0;
    while (read_count < states_num) {
        auto state = m_states_queue.Pop();
        if (!state) {
            break;
        }
        states[read_count++] = std::move(*state);
    }
    return read_count;
}

void GameController::Button(OrbisPadButtonDataOffset button, bool is_pressed) {
    std::lock_guard lock{m_state_mutex};
    m_state.OnButton(button, is_pressed);
    PushStateLocked();
}

void GameController::Axis(Input::Axis axis, int value, bool smooth) {
    std::lock_guard lock{m_state_mutex};
    const u64 timestamp = Libraries::Kernel::sceKernelGetProcessTime();
    m_state.OnAxis(axis, value, timestamp, smooth);
    PushStateLocked(timestamp);
}

void GameController::UpdateGyro(const float gyro[3]) {
    std::lock_guard lock{m_state_mutex};
    std::memcpy(gyro_buf, gyro, sizeof(gyro_buf));
}

void GameController::UpdateAcceleration(const float acceleration[3]) {
    std::lock_guard lock{m_state_mutex};
    std::memcpy(accel_buf, acceleration, sizeof(accel_buf));
}

void GameController::PollState() {
    std::lock_guard lock{m_state_mutex};
    PushStateLocked();
}

void GameController::SetShake(bool active) {
    std::lock_guard lock{m_state_mutex};
    m_spray_active = active;
    m_spray_start = Libraries::Kernel::sceKernelGetProcessTime();
    PushStateLocked();
}

void GameController::ResetOrientation() {
    std::lock_guard lock{m_state_mutex};
    m_state.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    m_last_orientation_update = 0;
    PushStateLocked();
}

void GameController::SetTouchpadState(int touch_index, bool touch_down, float x, float y) {
    if (touch_index < 0 || touch_index >= 2) {
        return;
    }

    std::lock_guard lock{m_state_mutex};
    const u64 timestamp = Libraries::Kernel::sceKernelGetProcessTime();
    const bool was_pressed = m_state.touchpad[0].state || m_state.touchpad[1].state;
    auto& touch = m_state.touchpad[touch_index];
    if (touch_down && !touch.state) {
        touch.ID = m_next_touch_id;
        m_next_touch_id = m_next_touch_id == 127 ? 1 : m_next_touch_id + 1;
    }
    m_state.OnTouchpad(touch_index, touch_down, x, y);
    const bool is_pressed = m_state.touchpad[0].state || m_state.touchpad[1].state;
    if (!was_pressed && is_pressed) {
        m_touch_down_timestamp = timestamp;
    } else if (was_pressed && !is_pressed) {
        m_touch_down_timestamp = 0;
    }
    PushStateLocked(timestamp);
}

void GameController::SetMotionTilt(int direction, bool active) {
    std::lock_guard lock{m_state_mutex};
    (direction > 0 ? m_tilt_left : m_tilt_right) = active;
    PushStateLocked();
}

void GameController::SetMotionShake(bool active) {
    std::lock_guard lock{m_state_mutex};
    const u64 timestamp = Libraries::Kernel::sceKernelGetProcessTime();
    if (active && m_shake_start == 0) {
        m_shake_start = timestamp;
    }
    m_shake_held = active;
    PushStateLocked(timestamp);
}

void GameController::ConnectController(SDL_Gamepad* pad) {
    std::lock_guard lock{m_state_mutex};
    m_sdl_gamepad = pad;
    if (override_colour) {
        SetLightBarRGB({});
    }
    m_states_queue.Clear();
    if (!m_state.connected) {
        ++m_state.connected_count;
        if (m_state.connected_count == 0) {
            m_state.connected_count = 1;
        }
    }
    m_state.connected = true;
    m_last_orientation_update = 0;
    PushStateLocked();
}

void GameController::DisconnectController() {
    std::lock_guard lock{m_state_mutex};
    m_states_queue.Clear();
    m_sdl_gamepad = nullptr;

    const u8 connected_count = m_state.connected_count;
    m_state = {};
    m_state.connected_count = connected_count;
    std::fill(gyro_buf, gyro_buf + 3, 0.0f);
    std::fill(accel_buf, accel_buf + 3, 0.0f);
    accel_buf[1] = 9.81f;
    m_next_touch_id = 1;
    m_touch_down_timestamp = 0;
    m_tilt_left = m_tilt_right = m_shake_held = false;
    m_shake_start = 0;
    m_synthetic_roll = m_synthetic_pitch = 0.0f;
    m_state.connected = false;
    m_last_orientation_update = 0;
    PushStateLocked();
}

void GameController::UpdateOrientationLocked(u64 timestamp) {
    if (m_last_orientation_update == 0 || timestamp <= m_last_orientation_update) {
        m_last_orientation_update = timestamp;
        return;
    }
    const float delta_time =
        static_cast<float>(timestamp - m_last_orientation_update) / 1'000'000.f;
    Libraries::Pad::OrbisFQuaternion orientation{};
    CalculateOrientation(m_state.angularVelocity, delta_time, m_state.orientation, orientation);
    m_state.orientation = orientation;
    m_last_orientation_update = timestamp;
}

void GameController::ApplySyntheticMotionLocked(u64 timestamp) {
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float tilt_angle = pi / 2.0f; // turn the controller sideways
    constexpr float tilt_speed = 7.85f;     // rad/s, a quarter turn in ~200 ms
    constexpr float shake_frequency = 9.0f; // Hz
    constexpr u64 shake_min_duration = 1'000'000;
    constexpr float shake_amplitude = 2.5f;   // g, on top of gravity
    constexpr float shake_wobble = 0.3f;      // rad of wrist pitch while shaking
    constexpr float max_wobble_speed = 20.0f; // rad/s
    constexpr float gravity = 9.81f;

    float shake = 0.0f;
    if (m_shake_start != 0) {
        const u64 elapsed = timestamp > m_shake_start ? timestamp - m_shake_start : 0;
        if (m_shake_held || elapsed < shake_min_duration) {
            shake = std::sin(2.0f * pi * shake_frequency * static_cast<float>(elapsed) / 1e6f);
        } else {
            m_shake_start = 0;
        }
    }
    const float target_roll = tilt_angle * (static_cast<int>(m_tilt_left) - m_tilt_right);
    const float target_pitch = shake_wobble * shake;
    if (m_shake_start == 0 && target_roll == 0.0f && m_synthetic_roll == 0.0f &&
        m_synthetic_pitch == 0.0f) {
        return; // inactive, keep the real (or mouse-emulated) sensor readings
    }

    // Move towards the target pose at a bounded speed and report the matching angular velocity,
    // so the orientation integrated from it in UpdateOrientationLocked follows the same pose.
    float dt = 0.0f;
    if (m_last_orientation_update != 0 && timestamp > m_last_orientation_update) {
        dt = static_cast<float>(timestamp - m_last_orientation_update) / 1e6f;
    }
    if (dt > 1.0f) {
        dt = 0.0f; // CalculateOrientation skips such gaps too
    }
    const auto step = [dt](float& angle, float target, float max_speed) {
        const float max_step = max_speed * dt;
        const float delta = std::clamp(target - angle, -max_step, max_step);
        angle = std::abs(target - angle) <= max_step ? target : angle + delta;
        return dt > 0.0f ? delta / dt : 0.0f;
    };
    const float pitch_velocity = step(m_synthetic_pitch, target_pitch, max_wobble_speed);
    const float roll_velocity = step(m_synthetic_roll, target_roll, tilt_speed);
    m_state.angularVelocity = {pitch_velocity, 0.0f, roll_velocity};

    // Gravity seen from the rolled and pitched controller, scaled by the shake.
    const float g = gravity * (1.0f + shake_amplitude * shake);
    const float cos_roll = std::cos(m_synthetic_roll);
    m_state.acceleration = {g * std::sin(m_synthetic_roll),
                            g * cos_roll * std::cos(m_synthetic_pitch),
                            -g * cos_roll * std::sin(m_synthetic_pitch)};
}

void GameController::PushStateLocked(u64 timestamp) {
    if (timestamp == 0) {
        timestamp = Libraries::Kernel::sceKernelGetProcessTime();
    }
    m_state.UpdateAxisSmoothing(timestamp);
    m_state.OnGyro(gyro_buf);
    if (m_spray_active) {
        // Emulate shaking a spray can: a strong ~7 Hz oscillation on every axis on top of the
        // real reading, so the shake registers whatever axis or orientation the game checks.
        constexpr float amplitude = 2.5f * 9.81f; // m/s^2, about 2.5 G
        constexpr float frequency = 7.0f;         // Hz
        const float t = static_cast<float>(timestamp - m_spray_start) / 1'000'000.f;
        const float wave = std::sin(2.0f * 3.14159265f * frequency * t) >= 0.0f ? 1.0f : -1.0f;
        const float shaken[3] = {accel_buf[0] + amplitude * wave, accel_buf[1] + amplitude * wave,
                                 accel_buf[2] + amplitude * wave};
        m_state.OnAccel(shaken);
    } else {
        m_state.OnAccel(accel_buf);
    }
    ApplySyntheticMotionLocked(timestamp);
    UpdateOrientationLocked(timestamp);
    m_state.time = timestamp;
    m_state.touch_time_since_held_down =
        m_touch_down_timestamp == 0 ? 0 : timestamp - m_touch_down_timestamp;
    m_states_queue.Push(m_state);
}

void GameController::SetLightBarRGB(u8 const r, u8 const g, u8 const b) {
    if (override_colour.has_value()) {
        if (m_sdl_gamepad) {
            SDL_SetGamepadLED(m_sdl_gamepad, override_colour->r, override_colour->g,
                              override_colour->b);
        }
        return;
    }
    colour = {r, g, b};
    if (m_sdl_gamepad != nullptr) {
        SDL_SetGamepadLED(m_sdl_gamepad, r, g, b);
    }
}

void GameController::SetLightBarRGB(Colour const c) {
    SetLightBarRGB(c.r, c.g, c.b);
}

Colour GameController::GetLightBarRGB() {
    return colour;
}

void GameController::PollLightColour() {
    if (m_sdl_gamepad != nullptr) {
        SDL_SetGamepadLED(m_sdl_gamepad, colour.r, colour.g, colour.b);
    }
}

void GameControllers::ResetLightbarColors() {
    for (auto& c : controllers) {
        auto const* u = UserManagement.GetUserByID(c->user_id);
        if (!u || !c->m_sdl_gamepad) {
            continue;
        }
        auto const i = u->user_color - 1;
        if (i < 0 || i > 3) {
            continue;
        }
        auto const& col = g_user_colours[i];
        c->override_colour = std::nullopt;
        c->SetLightBarRGB(col);
    }
}

bool GameController::SetVibration(u8 smallMotor, u8 largeMotor) {
    if (m_sdl_gamepad != nullptr && EmulatorSettings.IsVibrationEnabled()) {
        return SDL_RumbleGamepad(m_sdl_gamepad, (smallMotor / 255.0f) * 0xFFFF,
                                 (largeMotor / 255.0f) * 0xFFFF, -1);
    }
    return true;
}

static bool is_first_check = true;

#ifdef __linux__
namespace {
// The Steam Deck controller fills its IMU fields only when its IMU mode asks for raw data. Steam
// owns that setting and SDL's Deck driver never sets it, so the readings stay zero. A Steam launch
// also hides the controller behind a virtual pad without sensors. This keeps the raw stream on
// and, while the pad in use has no sensors, reads the motion from the controller directly.
void DeckImuThread(std::stop_token stop, GameController* controller) {
    Common::SetCurrentThreadName("shadPS4:DeckImu");
    SDL_hid_device* dev = nullptr;
    if (SDL_hid_device_info* infos = SDL_hid_enumerate(0x28de, 0x1205)) {
        for (auto* info = infos; info; info = info->next) {
            if (info->interface_number == 2) {
                dev = SDL_hid_open_path(info->path);
                break;
            }
        }
        SDL_hid_free_enumeration(infos);
    }
    if (!dev) {
        return;
    }
    LOG_INFO(Input, "Steam Deck controller found, keeping its IMU stream on");
    constexpr float accel_scale = 2.0f * SDL_STANDARD_GRAVITY / 32768.0f;
    constexpr float gyro_scale = 2000.0f * (SDL_PI_F / 180.0f) / 32768.0f;
    u64 last_mode_ms = 0;
    // The accelerometer saturates at +-2 G, half a DualShock 4's range, so a hard shake reads
    // flat at the rail. While an axis stays there, continue it with the slope it reached the
    // rail with, up to the DualShock 4's +-4 G. The firmware subtracts a small per-axis bias in
    // wrapping 16-bit math, so the rail reads near +-32767 but not exactly, and the same value
    // from both sides: the direction is the side the axis reached it from.
    constexpr s16 accel_rail = 32512;
    float accel_last[3]{};
    float accel_slope[3]{};
    float accel_sign[3]{};
    int accel_saturated[3]{};
    // The motion fed to the game, for analysing gestures: log/motion-<time>.csv, G and deg/s.
    // Only when SHAD_MOTION_LOG is set.
    const bool log_motion = std::getenv("SHAD_MOTION_LOG") != nullptr;
    std::FILE* csv = nullptr;
    bool touching = false;
    bool clicking = false;
    u64 last_flush_ms = 0;
    while (!stop.stop_requested()) {
        const u64 now_ms = SDL_GetTicks();
        if (now_ms - last_mode_ms >= 1000) {
            // ID_SET_SETTINGS_VALUES, one setting: SETTING_IMU_MODE = raw accel | raw gyro.
            u8 report[65] = {0, 0x87, 3, 48, 0x18, 0x00};
            SDL_hid_send_feature_report(dev, report, sizeof(report));
            last_mode_ms = now_ms;
        }
        // Deck state report: 4-byte header, then the accel and gyro words at payload offset 20.
        u8 data[64];
        const int size = SDL_hid_read_timeout(dev, data, sizeof(data), 100);
        if (size < 36 || data[2] != 0x09) {
            continue;
        }
        SDL_Gamepad* pad = controller->m_sdl_gamepad;
        if (!pad || SDL_GetNumGamepadTouchpads(pad) == 0) {
            // Low button word: touch left 0x80000 / right 0x100000, click left 0x20000 / right
            // 0x40000. Trackpads at payload offset 12: left x, y, right x, y. The touched one
            // (left first) is the DS4 touchpad, so swipes reach the game.
            u32 buttons;
            std::memcpy(&buttons, data + 8, sizeof(buttons));
            s16 pads[4];
            std::memcpy(pads, data + 16, sizeof(pads));
            const int side = (buttons & 0x80000) ? 0 : (buttons & 0x100000) ? 2 : -1;
            if (side >= 0) {
                const float x = (pads[side] + 32768.0f) / 65535.0f;
                const float y = 1.0f - (pads[side + 1] + 32768.0f) / 65535.0f;
                controller->SetTouchpadState(0, true, x, y);
                touching = true;
            } else if (touching) {
                controller->SetTouchpadState(0, false, 0.0f, 0.0f);
                touching = false;
            }
            const bool click = (buttons & 0x60000) != 0;
            if (click != clicking) {
                controller->Button(Libraries::Pad::OrbisPadButtonDataOffset::TouchPad, click);
                clicking = click;
            }
        }
        if (pad && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
            continue;
        }
        s16 raw[6];
        std::memcpy(raw, data + 24, sizeof(raw));
        float accel_raw[3];
        for (int i = 0; i < 3; ++i) {
            if (raw[i] > -accel_rail && raw[i] < accel_rail) {
                accel_slope[i] = raw[i] - accel_last[i];
                accel_last[i] = raw[i];
                accel_saturated[i] = 0;
                accel_raw[i] = raw[i];
                continue;
            }
            if (accel_saturated[i] == 0) {
                const float from = std::abs(accel_last[i]) > 8192.0f ? accel_last[i] : raw[i];
                accel_sign[i] = from > 0 ? 1.0f : -1.0f;
            }
            const float sign = accel_sign[i];
            const float slope = std::max(sign * accel_slope[i], 0.0f);
            const float value = sign * accel_last[i] + slope * ++accel_saturated[i];
            accel_raw[i] = sign * std::clamp(value, 32768.0f, 65536.0f);
        }
        // Same axes as SDL's Deck driver.
        const float accel[3] = {accel_raw[0] * accel_scale, accel_raw[2] * accel_scale,
                                -accel_raw[1] * accel_scale};
        const float gyro[3] = {raw[3] * gyro_scale, raw[5] * gyro_scale, -raw[4] * gyro_scale};
        controller->UpdateGyro(gyro);
        controller->UpdateAcceleration(accel);
        if (!log_motion) {
            continue;
        }
        if (!csv) {
            const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) /
                              fmt::format("motion-{}.csv", std::time(nullptr));
            csv = std::fopen(path.string().c_str(), "w");
            if (!csv) {
                continue;
            }
            std::fputs("t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n", csv);
        }
        constexpr float to_g = 1.0f / SDL_STANDARD_GRAVITY;
        constexpr float to_dps = 180.0f / SDL_PI_F;
        std::fprintf(csv, "%llu,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f\n",
                     static_cast<unsigned long long>(now_ms), accel[0] * to_g, accel[1] * to_g,
                     accel[2] * to_g, gyro[0] * to_dps, gyro[1] * to_dps, gyro[2] * to_dps);
        if (now_ms - last_flush_ms >= 1000) {
            std::fflush(csv);
            last_flush_ms = now_ms;
        }
    }
    if (csv) {
        std::fclose(csv);
    }
    SDL_hid_close(dev);
}

std::jthread g_deck_imu_thread;
} // Anonymous namespace
#endif

void GameControllers::TryOpenSDLControllers() {
    using namespace Libraries::UserService;
    int controller_count;
    SDL_JoystickID* new_joysticks = SDL_GetGamepads(&controller_count);
    LOG_INFO(Input, "{} controllers are currently connected", controller_count);

    std::unordered_set<SDL_JoystickID> assigned_ids;
    std::array<bool, 4> slot_taken{false, false, false, false};

    for (int i = 0; i < 4; i++) {
        SDL_Gamepad* pad = controllers[i]->m_sdl_gamepad;
        if (pad) {
            SDL_JoystickID id = SDL_GetGamepadID(pad);
            bool still_connected = false;
            for (int j = 0; j < controller_count; j++) {
                if (new_joysticks[j] == id) {
                    still_connected = true;
                    assigned_ids.insert(id);
                    slot_taken[i] = true;
                    break;
                }
            }
            if (!still_connected) {
                SDL_CloseGamepad(pad);
                controllers[i]->DisconnectController();
                controllers[i]->user_id = -1;
                slot_taken[i] = false;
            }
        }
    }

    for (int j = 0; j < controller_count; j++) {
        SDL_JoystickID id = new_joysticks[j];
        if (assigned_ids.contains(id))
            continue;

        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad) {
            continue;
        }

        for (int i = 0; i < 4; i++) {
            if (!slot_taken[i]) {
                auto u = UserManagement.GetUserByPlayerIndex(i + 1);
                if (!u) {
                    LOG_INFO(Input, "User {} not found", i + 1);
                    continue; // for now, if you don't specify who Player N is in the config,
                              // Player N won't be registered at all
                }
                auto* c = controllers[i];
                const char* name = SDL_GetGamepadName(pad);
                LOG_INFO(Input, "Gamepad registered for slot {}! Handle: {}, name: {}, Steam: {}",
                         i, SDL_GetGamepadID(pad), name ? name : "",
                         SDL_GetGamepadSteamHandle(pad));
                slot_taken[i] = true;
                c->user_id = u->user_id;
                UserManagement.LoginUser(u, i + 1);
                c->ConnectController(pad);
                if (EmulatorSettings.IsMotionControlsEnabled() && PromptPacks::MotionAllowed()) {
                    if (SDL_SetGamepadSensorEnabled(c->m_sdl_gamepad, SDL_SENSOR_GYRO, true)) {
                        const float poll_rate =
                            SDL_GetGamepadSensorDataRate(c->m_sdl_gamepad, SDL_SENSOR_GYRO);
                        LOG_INFO(Input, "Gyro initialized, poll rate: {}", poll_rate);
                    } else {
                        LOG_ERROR(Input, "Failed to initialize gyro controls for gamepad {}",
                                  c->user_id);
                    }
                    if (SDL_SetGamepadSensorEnabled(c->m_sdl_gamepad, SDL_SENSOR_ACCEL, true)) {
                        const float poll_rate =
                            SDL_GetGamepadSensorDataRate(c->m_sdl_gamepad, SDL_SENSOR_ACCEL);
                        LOG_INFO(Input, "Accel initialized, poll rate: {}", poll_rate);
                    } else {
                        LOG_ERROR(Input, "Failed to initialize accel controls for gamepad {}",
                                  c->user_id);
                    }
                }
                break;
            }
        }
    }
#ifdef __linux__
    if (EmulatorSettings.IsMotionControlsEnabled() && !g_deck_imu_thread.joinable()) {
        g_deck_imu_thread = std::jthread(DeckImuThread, controllers[0]);
    }
#endif
    if (is_first_check) [[unlikely]] {
        is_first_check = false;
        if (controller_count == 0) {
            auto u = UserManagement.GetUserByPlayerIndex(1);
            controllers[0]->user_id = u->user_id;
            controllers[0]->ConnectController(nullptr);
            UserManagement.LoginUser(u, 1);
        }
    }
    SDL_free(new_joysticks);
}
u8 GameControllers::GetGamepadIndexFromJoystickId(SDL_JoystickID id) {
    auto g = SDL_GetGamepadFromID(id);
    ASSERT(g != nullptr);
    for (int i = 0; i < 5; i++) {
        if (controllers[i]->m_sdl_gamepad == g) {
            return i;
        }
    }
    // LOG_TRACE(Input, "Gamepad index: {}", index);
    return -1;
}

std::optional<u8> GameControllers::GetControllerIndexFromUserID(s32 user_id) {
    auto const u = UserManagement.GetUserByID(user_id);
    if (!u) {
        return std::nullopt;
    }
    return u->player_index - 1;
}

std::optional<u8> GameControllers::GetControllerIndexFromControllerID(s32 controller_id) {
    if (controller_id < 1 || controller_id > 5) {
        return std::nullopt;
    }
    return controller_id - 1;
}

} // namespace Input
