/*
 * Copyright (C) 2018-2020 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "LightService"

#include <log/log.h>

#include "Light.h"

#include <fstream>

#define LED_PATH_LEFT "/sys/class/leds/left/"
#define LED_PATH_WHITE "/sys/class/leds/white/"

#define BREATH              "breath"
#define BRIGHTNESS          "brightness"
#define MAX_BRIGHTNESS      "max_brightness"

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {

// Initialize static members
std::mutex Light::sLock;
std::vector<LightBackend> Light::sBackends;
std::string Light::sBasePath;
int Light::sMaxBrightness = 255;

// Anonymous namespace for truly local helper functions
namespace {

/*
 * Write value to path and close file.
 */
static void set(const std::string& path, const std::string& value) {
    std::ofstream file(path);
    if (!file.is_open()) {
        ALOGW("failed to write %s to %s", value.c_str(), path.c_str());
        return;
    }
    file << value;
}

static void set(const std::string& path, int value) {
    set(path, std::to_string(value));
}

static uint32_t getBrightness(const LightState& state) {
    uint32_t alpha, red, green, blue;

    /*
     * Extract brightness from AARRGGBB.
     */
    alpha = (state.color >> 24) & 0xFF;
    red = (state.color >> 16) & 0xFF;
    green = (state.color >> 8) & 0xFF;
    blue = state.color & 0xFF;

    /*
     * Scale RGB brightness using Alpha brightness.
     */
    red = red * alpha / 0xFF;
    green = green * alpha / 0xFF;
    blue = blue * alpha / 0xFF;

    return (77 * red + 150 * green + 29 * blue) >> 8;
}

static inline uint32_t scaleBrightness(uint32_t brightness, uint32_t maxBrightness) {
    if (brightness == 0) {
        return 0;
    }
    return (brightness - 1) * (maxBrightness - 1) / (0xFF - 1) + 1;
}

static inline uint32_t getScaledBrightness(const LightState& state, uint32_t maxBrightness) {
    return scaleBrightness(getBrightness(state), maxBrightness);
}

static inline bool isStateLit(const LightState& state) {
    return state.color & 0x00ffffff;
}

static inline bool isStateEqual(const LightState& first, const LightState& second) {
    return first.color == second.color && first.flashMode == second.flashMode &&
           first.flashOnMs == second.flashOnMs && first.flashOffMs == second.flashOffMs &&
           first.brightnessMode == second.brightnessMode;
}

}  // anonymous namespace

// Constructor
Light::Light() {
    std::lock_guard<std::mutex> lock(sLock);

    // Initialize static members only once
    if (sBackends.empty()) {
        // Determine the correct base path for LEDs once to improve performance.
        std::ifstream left_path(std::string(LED_PATH_LEFT) + BRIGHTNESS);
        if (left_path.good()) {
            sBasePath = LED_PATH_LEFT;
        } else {
            sBasePath = LED_PATH_WHITE;
        }
        ALOGI("Using LED base path: %s", sBasePath.c_str());

        // Read max brightness once from path and cache it.
        std::ifstream file(sBasePath + MAX_BRIGHTNESS);
        if (file.is_open()) {
            file >> sMaxBrightness;
        } else {
            ALOGW("Failed to read max brightness, defaulting to 255");
            sMaxBrightness = 255;
        }

        /* Keep sorted in the order of importance. */
        sBackends.emplace_back(Type::ATTENTION, &Light::handleNotification);
        sBackends.emplace_back(Type::NOTIFICATIONS, &Light::handleNotification);
        sBackends.emplace_back(Type::BATTERY, &Light::handleNotification);
    }
}

// HIDL methods
Return<Status> Light::setLight(Type type, const LightState& state) {
    /* Lock mutex until light state is updated. */
    std::lock_guard<std::mutex> lock(sLock);

    LightStateHandler handler = findHandler(type);
    if (!handler) {
        /* If no handler has been found, then the type is not supported. */
        return Status::LIGHT_NOT_SUPPORTED;
    }

    /* Find the old state of the current handler. */
    LightState oldState = findLitState(handler);

    /* Update the cached state value for the current type. */
    updateState(type, state);

    /* Find the new state of the current handler. */
    LightState newState = findLitState(handler);

    if (isStateEqual(oldState, newState)) {
        return Status::SUCCESS;
    }

    handler(newState);

    return Status::SUCCESS;
}

Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
    std::vector<Type> types;
    {
        std::lock_guard<std::mutex> lock(sLock);
        for (const auto& backend : sBackends) {
            types.push_back(backend.type);
        }
    }
    _hidl_cb(types);
    return Void();
}

// Private static methods of Light class
LightStateHandler Light::findHandler(Type type) {
    for (const auto& backend : sBackends) {
        if (backend.type == type) {
            return backend.handler;
        }
    }
    return nullptr;
}

LightState Light::findLitState(LightStateHandler handler) {
    LightState emptyState;
    for (const auto& backend : sBackends) {
        if (backend.handler == handler) {
            if (isStateLit(backend.state)) {
                return backend.state;
            }
            emptyState = backend.state;
        }
    }
    return emptyState;
}

void Light::updateState(Type type, const LightState& state) {
    for (auto& backend : sBackends) {
        if (backend.type == type) {
            backend.state = state;
            return;
        }
    }
}

void Light::handleNotification(const LightState& state) {
    uint32_t notificationBrightness = getScaledBrightness(state, sMaxBrightness);

    /* Disable breathing or blinking before setting a new state */
    set(sBasePath + BREATH, 0);
    set(sBasePath + BRIGHTNESS, 0);

    if (!notificationBrightness) {
        return; // Turn off the light
    }

    switch (state.flashMode) {
        case Flash::HARDWARE:
        case Flash::TIMED:
            /* Breathing / Pulsing */
            // Based on kernel driver analysis, setting brightness here would
            // disable the breath mode. The driver uses a predefined brightness
            // (usually max) for the breath effect.
            set(sBasePath + BREATH, 1);
            break;
        case Flash::NONE:
        default:
            /* Solid on */
            set(sBasePath + BRIGHTNESS, notificationBrightness);
    }
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
