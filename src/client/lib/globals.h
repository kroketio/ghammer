#pragma once

#include <string>

#include <QObject>
#include <QDebug>
#include <QFileInfo>

#include "client/vulkan/engine3d_fwd.h"

namespace g {
  Q_NAMESPACE

  enum class EditorMode {
    OBJECT = 0,
    EDIT,
    MATERIAL
  };
  Q_ENUM_NS(EditorMode)

  extern vk::Engine3D *engine3D;
  extern vk::TextureManager *textureManager;
  extern EditorMode editorMode;
}
