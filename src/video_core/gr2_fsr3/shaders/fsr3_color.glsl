// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Colour helpers of the module's passes: the sRGB encode of the Gravity Rush 2 antialias pass, the
// luma its tonemap pass writes to alpha, and the component order of a colour target.

vec3 Fsr3SrgbEncode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

vec3 Fsr3SrgbDecode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

float Fsr3Luma(vec3 linear_color) {
    return dot(linear_color, vec3(0.299, 0.587, 0.114));
}

// Takes component (order >> 8 * i) & 0xff of c into component i.
vec4 Fsr3Reorder(vec4 c, uint order) {
    return vec4(c[order & 0xffu], c[(order >> 8) & 0xffu], c[(order >> 16) & 0xffu],
                c[order >> 24]);
}
