#pragma once

// Builds the scene shader's textures from a texture's map layout. Pure QImage code (unit-tested).
// ORM texture (binding 6): R = AO, G = roughness, B = metallic; the shader multiplies roughness / metallic by the
// material parameters like Godot's StandardMaterial3D. A missing source writes 255, as Godot's empty slots are white.

#include <QImage>

#include "shared/models/material_props.h"

namespace eng {

// Grey = mean of R, G, B, like Godot's TEXTURE_CHANNEL_GRAYSCALE.
uchar channelValue(const QImage &rgba, int x, int y, matprops::Channel ch);

// A null source is 255; sources are scaled to the largest one. Null image when all three are null.
QImage composeOrm(const QImage &ao, matprops::Channel aoChannel, const QImage &roughness,
                  matprops::Channel roughnessChannel, const QImage &metallic, matprops::Channel metallicChannel);

// Alpha from the opacity map's red channel; `albedo` unchanged without one.
QImage mergeOpacity(const QImage &albedo, const QImage &opacity);

// 64x64 purple/black checkerboard: texture missing or server unreachable.
QImage errorChecker();

} // namespace eng
