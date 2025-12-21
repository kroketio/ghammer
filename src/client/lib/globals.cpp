#include "client/lib/globals.h"

#include "client/ctx.h"

namespace g {
  vk::Engine3D *engine3D = nullptr;
  vk::TextureManager *textureManager = nullptr;
  EditorMode editorMode = EditorMode::OBJECT;
}
