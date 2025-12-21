// Every built-in preference, in dialog order. Pre-registry keys (CameraRenderDistance, ...) keep their names.

#include "preferences.h"

#include <utility>

#include "client/ui/external_apps.h"
#include "shared/lib/config.h"

namespace prefs {

void installRenderPresets(Preferences &p);

namespace {

Def boolDef(const char *key, const char *cat, const char *label, bool def, const char *tip = "") {
  Def d;
  d.key = QString::fromLatin1(key);
  d.category = QString::fromLatin1(cat);
  d.label = QString::fromUtf8(label);
  d.tooltip = QString::fromUtf8(tip);
  d.type = Type::Bool;
  d.defaultValue = def;
  return d;
}

Def intDef(const char *key, const char *cat, const char *label, int def, int lo, int hi,
           const char *suffix = "", const char *tip = "") {
  Def d;
  d.key = QString::fromLatin1(key);
  d.category = QString::fromLatin1(cat);
  d.label = QString::fromUtf8(label);
  d.tooltip = QString::fromUtf8(tip);
  d.type = Type::Int;
  d.defaultValue = def;
  d.minValue = lo;
  d.maxValue = hi;
  d.suffix = QString::fromUtf8(suffix);
  return d;
}

Def dblDef(const char *key, const char *cat, const char *label, double def, double lo, double hi,
           double step, int decimals, const char *suffix = "", const char *tip = "") {
  Def d;
  d.key = QString::fromLatin1(key);
  d.category = QString::fromLatin1(cat);
  d.label = QString::fromUtf8(label);
  d.tooltip = QString::fromUtf8(tip);
  d.type = Type::Double;
  d.defaultValue = def;
  d.minValue = lo;
  d.maxValue = hi;
  d.step = step;
  d.decimals = decimals;
  d.suffix = QString::fromUtf8(suffix);
  return d;
}

Def colorDef(const char *key, const char *cat, const char *label, const char *def,
             const char *tip = "") {
  Def d;
  d.key = QString::fromLatin1(key);
  d.category = QString::fromLatin1(cat);
  d.label = QString::fromUtf8(label);
  d.tooltip = QString::fromUtf8(tip);
  d.type = Type::Color;
  d.defaultValue = QColor(QString::fromLatin1(def)).name(QColor::HexArgb);
  return d;
}

Def choiceDef(const char *key, const char *cat, const char *label, const QStringList &labels,
              const QVariantList &values, const QVariant &def, const char *tip = "") {
  Def d;
  d.key = QString::fromLatin1(key);
  d.category = QString::fromLatin1(cat);
  d.label = QString::fromUtf8(label);
  d.tooltip = QString::fromUtf8(tip);
  d.type = Type::Choice;
  d.choices = labels;
  d.choiceValues = values;
  d.defaultValue = def;
  return d;
}

// A project or level setting instead of an application one.
Def inScope(Def d, Scope scope) {
  d.scope = scope;
  return d;
}

Def pathDef(const char *key, const char *cat, const char *label, const char *def, Type type,
            const char *tip = "") {
  Def d;
  d.key = QString::fromLatin1(key);
  d.category = QString::fromLatin1(cat);
  d.label = QString::fromUtf8(label);
  d.tooltip = QString::fromUtf8(tip);
  d.type = type;
  d.defaultValue = QString::fromUtf8(def);
  return d;
}

} // namespace

void registerBuiltins(Preferences &p) {
  p.declare(intDef("General/UndoLimit", "General", "Undo history length", 250, 10, 5000, " steps",
                   "How many edits are kept for undo. Older steps are dropped."));
  p.declare(intDef("General/AutosaveMinutes", "General", "Autosave interval", 5, 0, 120, " min",
                   "0 disables autosave. Autosaves never overwrite the map itself."));
  p.declare(intDef("General/AutosaveBackups", "General", "Autosave backups kept", 5, 1, 50));
  {
    const QStringList labels{"1×", "1.25×", "1.5×", "1.75×", "2×"};
    const QVariantList values{1.0, 1.25, 1.5, 1.75, 2.0};
    Def ui = choiceDef("General/UiScale", "General", "UI scale", labels, values, 1.0,
                       "Scales the whole interface: text, icons, spacing and viewport overlays. "
                       "1× is normal desktop density; a 4K screen wants about 2×.");
    ui.buttons = true;
    p.declare(ui);
    Def text = choiceDef("General/FontScale", "General", "Text size", labels, values, 1.0,
                         "Text size on top of the UI scale; icons and spacing stay as they are.");
    text.buttons = true;
    p.declare(text);
  }
  p.declare(choiceDef("General/LogLevel", "General", "Console log level",
                      {"Debug", "Info", "Warning", "Critical"}, {0, 1, 2, 3}, 1,
                      "Messages below this level are hidden in the console panel."));

  p.declare(dblDef("Viewport/FieldOfView", "Viewport", "Field of view", 70.0, 30.0, 120.0, 1.0, 0, "°",
                   "Vertical field of view of the 3D view."));
  p.declare(dblDef("Viewport/MouseSensitivity", "Viewport", "Mouse look sensitivity", 0.10, 0.01, 1.0,
                   0.01, 2));
  p.declare(boolDef("Viewport/InvertY", "Viewport", "Invert mouse Y", false));
  p.declare(boolDef("View/ShowCollision", "Viewport", "Show clip brushes in the 3D view", true));
  p.declare(boolDef("View/ShowTriggers", "Viewport", "Show trigger brushes in the 3D view", true,
                    "Trigger volumes are drawn see-through (orange) and never lit or shadowed."));
  p.declare(boolDef("View/ShowIOLines", "Viewport", "Show I/O connections", true,
                    "Lines from each entity to the entities its outputs target, in the 3D and 2D views."));
  p.declare(boolDef("View/IOLinesAll", "Viewport", "Show every I/O connection", true,
                    "Off: only the connections of the selection. On: all, with the selection's highlighted."));
  p.declare(boolDef("View/IOLabels", "Viewport", "Label the selection's I/O connections", true));
  p.declare(boolDef("Viewport/FlyRequiresRightMouse", "Viewport", "WASD flies only while the right mouse button is held",
                    true, "Like Unity/Unreal/Godot. Frees letter keys for tool shortcuts when not flying."));
  p.declare(dblDef("Viewport/FlySpeed", "Viewport", "Default fly speed", 55.0, 1.0, 2000.0, 5.0, 0,
                   " u/s", "Initial WASD speed. The mouse wheel changes it while flying."));
  p.declare(dblDef("Viewport/FlySpeedMin", "Viewport", "Minimum fly speed", 6.25, 1.0, 1000.0, 1.25, 2, " u/s"));
  p.declare(dblDef("Viewport/FlySpeedMax", "Viewport", "Maximum fly speed", 300.0, 10.0, 5000.0, 10.0, 0, " u/s"));
  p.declare(intDef("Viewport/FlySpeedSteps", "Viewport", "Wheel notches from min to max speed", 10, 4, 40, "",
                   "The slow half of the range takes fewer notches than the fast half."));
  p.declare(dblDef("Viewport/FlyShiftMultiplier", "Viewport", "Shift speed multiplier", 3.0, 1.0, 10.0, 0.5, 1, "×",
                   "Holding Shift while flying multiplies the speed."));
  p.declare(dblDef("Viewport/FlySmoothing", "Viewport", "Fly acceleration smoothing", 0.0, 0.0, 0.95, 0.05, 2, "",
                   "0 = instant start/stop, higher = smoother acceleration."));
  p.declare(dblDef("Viewport/NearPlane", "Viewport", "Near clip plane", 0.1, 0.01, 10.0, 0.01, 2, " u"));
  p.declare(boolDef("Viewport/OrbitAroundSelection", "Viewport", "Alt+drag orbits around the selection", true));
  p.declare(boolDef("Viewport/ShowViewGizmo", "Viewport", "Show the axis gizmo in the 3D view", true));
  p.declare(intDef("Viewport/EntityIconSize", "Viewport", "Entity icon size", 60, 16, 192, " px",
                   "Size of the light / entity icons in the 3D view"));
  p.declare(boolDef("Viewport/ShowFps", "Viewport", "Show frame time in the status bar", true));

  p.declare(choiceDef("Render/Preset", "Rendering", "Quality preset",
                      {"Low", "Medium", "High", "Ultra", "Custom"}, {0, 1, 2, 3, 4}, 2,
                      "Sets the options below in one go. Changing any of them switches to Custom."));
  p.declare(dblDef("CameraRenderDistance", "Rendering", "Camera render distance", 7168.0, 1024.0, 40960.0,
                   256.0, 0, " u"));
  p.declare(dblDef("LightRenderDistance", "Rendering", "Light render distance", 6144.0, 1024.0, 40960.0, 256.0,
                   0, " u"));
  p.declare(dblDef("ShadowRenderDistance", "Rendering", "Shadow distance", 10240.0, 1024.0, 40960.0, 256.0, 0,
                   " u", "How far from the camera sun shadows are drawn."));
  p.declare(choiceDef("Render/Msaa", "Rendering", "MSAA", {"Off", "2×", "4×", "8×"}, {1, 2, 4, 8}, 2));
  p.declare(choiceDef("Render/MaxTextureSize", "Rendering", "Maximum texture size",
                      {"512", "1024", "2048", "4096", "8192"}, {512, 1024, 2048, 4096, 8192}, 4096,
                      "Larger textures are downscaled on load. Applies to textures loaded afterwards."));
  p.declare(boolDef("Render/ContinuousRedraw", "Rendering", "Redraw continuously",
                    false, "Off: the 3D view only redraws when something changes (saves GPU/battery)."));
  p.declare(choiceDef("Render/ShadowMapSize", "Rendering/Shadows", "Sun shadow resolution",
                      {"1024", "2048", "4096", "8192"}, {1024, 2048, 4096, 8192}, 4096,
                      "Per cascade."));
  p.declare(intDef("Render/ShadowCascades", "Rendering/Shadows", "Sun shadow cascades", 4, 1, 4));
  p.declare(dblDef("Render/CascadeSplitLambda", "Rendering/Shadows", "Cascade split distribution", 0.75, 0.0,
                   1.0, 0.05, 2, "", "0 = linear splits, 1 = logarithmic splits."));
  p.declare(choiceDef("Render/ShadowFilter", "Rendering/Shadows", "Shadow filtering",
                      {"Hard", "PCF", "PCSS (soft)"}, {0, 1, 2}, 1));
  p.declare(choiceDef("Render/ShadowTaps", "Rendering/Shadows", "Shadow filter samples",
                      {"8", "16", "32"}, {8, 16, 32}, 16));
  p.declare(choiceDef("Render/LocalShadowAtlas", "Rendering/Shadows", "Light shadow atlas size",
                      {"2048", "4096", "8192"}, {2048, 4096, 8192}, 4096));
  p.declare(intDef("Render/MaxShadowedLights", "Rendering/Shadows", "Max shadowed lights", 8, 0, 32));
  p.declare(boolDef("Render/ShowCascades", "Rendering/Shadows", "Debug: tint shadow cascades", false));
  // Godot's Environment tone mappers, in Godot's order and with its default (Linear).
  p.declare(choiceDef("Render/Tonemapper", "Rendering/Post", "Tone mapping",
                      {"Linear (Godot's default)", "Reinhard", "Filmic", "ACES", "AgX"}, {0, 1, 2, 3, 4}, 0));
  p.declare(boolDef("Render/Specular", "Rendering/Post", "Specular reflections", true,
                    "Highlights and the ambient reflection in the 3D view (View › Specular Reflections)"));
  p.declare(dblDef("Render/SpecularStrength", "Rendering/Post", "Specular strength", 2.5, 0.0, 4.0, 0.25, 2, "×",
                   "Intensity of highlights and reflections in the 3D view"));
  p.declare(boolDef("Render/NormalMaps", "Rendering/Post", "Normal maps", true,
                    "Normal maps in the 3D view (View › Normal Maps)"));
  p.declare(dblDef("Render/NormalStrength", "Rendering/Post", "Normal map strength", 1.0, 0.0, 4.0, 0.25, 2, "×",
                   "Depth of normal maps in the 3D view"));
  p.declare(boolDef("Render/Ssao", "Rendering/Post", "Ambient occlusion (SSAO)", true));
  p.declare(choiceDef("Render/SsaoQuality", "Rendering/Post", "SSAO quality",
                      {"Low (8)", "Medium (16)", "High (32)"}, {8, 16, 32}, 16));
  p.declare(boolDef("Render/SsaoHalfRes", "Rendering/Post", "SSAO at half resolution", true));
  p.declare(boolDef("Render/Fxaa", "Rendering/Post", "FXAA", true));
  p.declare(boolDef("Render/Sky", "Rendering/Post", "Draw sky", true));

  p.declare(dblDef("Grid/AngleSnap", "Grid & Snap", "Rotation snap", 15.0, 1.0, 90.0, 1.0, 0, "°"));
  p.declare(boolDef("Grid/AngleSnapEnabled", "Grid & Snap", "Snap rotations", true));

  p.declare(colorDef("Color/Background3D", "Colors", "3D background (no sky)", "#141825"));

  p.declare(dblDef("Gizmo/Size", "Gizmos", "Gizmo size", 120.0, 40.0, 400.0, 5.0, 0, " px"));
  p.declare(dblDef("Gizmo/LabelSize", "Gizmos", "Dimension label size", 28.0, 10.0, 64.0, 1.0, 0, " px"));
  p.declare(boolDef("Gizmo/ShowLabels", "Gizmos", "Show dimension labels", true));
  p.declare(dblDef("Gizmo/WireThickness", "Gizmos", "Selection wire thickness", 1.5, 0.5, 6.0, 0.25, 2, " px"));

  p.declare(pathDef("Brush/DefaultMaterial", "Brushes", "Default material", "dev/dev_measurewall01d",
                    Type::String));
  p.declare(dblDef("Brush/DefaultTextureScale", "Brushes", "Default texture scale", 0.25, 0.01, 16.0, 0.0625, 4));
  p.declare(choiceDef("Brush/Shape", "Brushes", "Shape for new brushes",
                      {"Box", "Wedge", "Cylinder", "Cone", "Spike", "Sphere", "Stairs", "Arch", "Pyramid"},
                      {0, 1, 2, 3, 4, 5, 6, 7, 8}, 0));
  p.declare(intDef("Brush/CylinderSides", "Brushes", "Default cylinder sides", 16, 3, 64));
  p.declare(intDef("Brush/StairSteps", "Brushes", "Default stair steps", 8, 2, 64));
  p.declare(choiceDef("Brush/StairStyle", "Brushes", "Stair underside", {"Solid", "Planks (zigzag)", "Slope"},
                      {0, 1, 2}, 0));
  p.declare(choiceDef("Brush/StairSizing", "Brushes", "Stair step count", {"From step size", "Fixed count"}, {0, 1}, 0));
  p.declare(dblDef("Brush/StairStepHeight", "Brushes", "Stair step height (units)", 8.0, 1.0, 256.0, 1.0, 0));
  p.declare(dblDef("Brush/StairStepDepth", "Brushes", "Stair tread depth (units)", 8.0, 1.0, 512.0, 1.0, 0));
  p.declare(dblDef("Brush/StairThickness", "Brushes", "Stair slope thickness (units)", 16.0, 1.0, 512.0, 1.0, 0));
  p.declare(boolDef("Brush/StairThinPlanks", "Brushes", "Stair planks: thin planks", false));
  p.declare(dblDef("Brush/StairPlankThickness", "Brushes", "Stair plank thickness (units)", 4.0, 0.5, 64.0, 0.5, 1));
  p.declare(boolDef("Brush/StairNosing", "Brushes", "Stair step overshoot", false));
  p.declare(dblDef("Brush/StairNosingDepth", "Brushes", "Stair overshoot depth (units)", 6.0, 0.5, 64.0, 0.5, 1));
  p.declare(dblDef("Brush/StairNosingHeight", "Brushes", "Stair overshoot height (units)", 2.0, 0.5, 64.0, 0.5, 1));
  p.declare(boolDef("Brush/StairCollision", "Brushes", "Stairs: clip-brush ramp (func_clip)", true));
  p.declare(boolDef("Brush/StairSides", "Brushes", "Stairs: side brushes", false));
  p.declare(dblDef("Brush/StairSideWidth", "Brushes", "Stair side width (units)", 4.0, 0.5, 128.0, 0.5, 1));
  p.declare(dblDef("Brush/StairSideHeight", "Brushes", "Stair side height above the steps (units)", 8.0, 0.0, 256.0, 1.0,
                   0));
  p.declare(intDef("Brush/StairDirection", "Brushes", "Stair climb direction (top view)", 2, 0, 3));
  p.declare(intDef("Brush/ArchSegments", "Brushes", "Default arch segments", 8, 2, 64));
  p.declare(choiceDef("Brush/ArchProfile", "Brushes", "Arch profile", {"Round", "Segmental", "Pointed", "Elliptical", "Tudor"},
                      {0, 1, 2, 3, 4}, 0));
  p.declare(choiceDef("Brush/ArchMode", "Brushes", "Arch kind", {"Ring", "Archway (wall with opening)", "Carve opening"},
                      {0, 1, 2}, 1));
  // Shape tools remember their last settings (vk_shape_tools.cpp); Brush/CylinderSides is also the plain shape's.
  p.declare(intDef("Brush/CapsuleSides", "Hidden", "Capsule sides", 16, 3, 64));
  p.declare(intDef("Brush/PyramidSides", "Hidden", "Pyramid sides", 4, 3, 64));
  p.declare(intDef("Brush/CapsuleCaps", "Hidden", "Capsule end rings", 4, 1, 16));
  p.declare(dblDef("Brush/CylinderWall", "Hidden", "Cylinder wall", 0.0, 0.0, 4096.0, 1.0, 0, " u"));
  p.declare(boolDef("Brush/CylinderSmooth", "Hidden", "Smooth cylinders", true));
  p.declare(boolDef("Brush/CapsuleSmooth", "Hidden", "Smooth capsules", true));
  p.declare(boolDef("Brush/PyramidSmooth", "Hidden", "Smooth pyramids", false));
  p.declare(boolDef("Brush/ArchSmooth", "Brushes", "Smooth arches", true,
                    "New arches shade their curve as one smooth surface"));
  p.declare(dblDef("Brush/ArchThickness", "Brushes", "Arch ring depth", 16.0, 1.0, 512.0, 1.0, 0, " u"));
  p.declare(dblDef("Brush/ArchJamb", "Brushes", "Archway jamb / crown", 0.0, 0.0, 512.0, 1.0, 0, " u",
                   "0: the curve springs from the box's sides and touches its top"));
  p.declare(boolDef("Entity/DeleteBrushesWithEntity", "Brushes", "Deleting a brush entity deletes its brushes", true,
                    "Off: the brushes go back to the world instead."));
  p.declare(boolDef("IO/RenameUpdatesReferences", "Entities", "Renaming an entity updates connections to it", true,
                    "Outputs and target properties that named the old name follow the new one."));
  p.declare(dblDef("IO/SimTimeScale", "Entities", "I/O simulator speed", 1.0, 0.05, 20.0, 0.25, 2, "×"));
  p.declare(pathDef("Godot/ExportDir", "Entities", "Godot export folder", "", Type::Path,
                    "Where File › Export I/O to Godot writes the map script (usually res://maps in your project)."));
  p.declare(boolDef("Brush/VertexStrict", "Brushes", "Strict vertex editing", false,
                    "On: a vertex move that would fold a corner into the brush is refused instead of "
                    "rebuilt as the convex hull."));
  p.declare(boolDef("Brush/SubtractRemovesCutter", "Brushes", "Subtract removes the cutting brush", true,
                    "Off: the cutter stays (Hammer's Carve)."));
  p.declare(boolDef("Brush/HollowOutward", "Brushes", "Hollow grows walls outward", false,
                    "On: the brush's inside stays the room size and the walls are added around it."));
  p.declare(dblDef("Brush/HollowThickness", "Brushes", "Default hollow wall thickness", 16.0, 1.0, 512.0, 1.0, 0,
                   " u"));

  p.declare(choiceDef("Viewport/ZenBox", "Viewport", "Zen mode box",
                      {"256 × 256 × 256", "512 × 512 × 256", "512 × 512 × 512", "768 × 768 × 384",
                       "768 × 768 × 768", "1024 × 1024 × 512", "1024 × 1024 × 1024", "1536 × 1536 × 768",
                       "2048 × 2048 × 1024", "4096 × 4096 × 2048"},
                      {"256x256x256", "512x512x256", "512x512x512", "768x768x384", "768x768x768", "1024x1024x512",
                       "1024x1024x1024", "1536x1536x768", "2048x2048x1024", "4096x4096x2048"},
                      "768x768x768", "What zen mode shows in the 2D views: a box this size around the 3D camera"));

  // Project (.ghammer/settings.json)
  // Defaults are the Layout Designer's built-in sizes (eng::layout::Config).
  {
    const auto layoutInt = [&p](const char *key, const char *label, int def, const char *tip = "") {
      p.declare(inScope(intDef(key, "Project/Layout Designer", label, def, 1, 4096, " u", tip), Scope::Project));
    };
    layoutInt("Layout/Wall", "Wall thickness", 10);
    layoutInt("Layout/Floor", "Floor thickness", 16);
    layoutInt("Layout/Ceiling", "Ceiling thickness", 16);
    layoutInt("Layout/RoomHeight", "Room height", 128);
    layoutInt("Layout/CorridorWidth", "Corridor width", 128);
    layoutInt("Layout/CorridorHeight", "Corridor height", 128);
    layoutInt("Layout/DoorWidth", "Door width", 64);
    layoutInt("Layout/DoorHeight", "Door height", 112);
    layoutInt("Layout/VentWidth", "Vent width", 48);
    layoutInt("Layout/VentHeight", "Vent height", 48);
    layoutInt("Layout/VentWall", "Vent wall thickness", 8);
  }
  p.declare(inScope(boolDef("Export/AutoExport", "Project/Godot Export", "Export on save", true,
                            "Save exports the levels it wrote to the auto-export target."),
                    Scope::Project));
  p.declare(inScope(pathDef("Export/Target", "Project/Godot Export", "Auto-export target", "game", Type::Path,
                            "The Godot project Save exports to: game (this project's own) or another "
                            "project's folder."),
                    Scope::Project));
  p.declare(inScope(boolDef("Export/WriteAddon", "Project/Godot Export", "Also write the gHammer runtime addon",
                            true, "Copies addons/ghammer into the Godot project."),
                    Scope::Project));

  // New Godot projects (written once into project.godot)
  p.declare(intDef("Godot/NewProjectResolution", "Godot", "New project window size", 75, 10, 100, " %",
                   "A new Godot project's viewport size: this share of the screen's resolution. Godot owns the "
                   "setting afterwards."));
  p.declare(intDef("Godot/NewProjectMaxFps", "Godot", "New project max FPS", 120, 0, 1000, " fps",
                   "A new Godot project's frame rate cap (0 = none). Godot owns the setting afterwards."));
  p.declare(inScope(pathDef("Godot/Executable", "Project/Godot", "Godot executable", "", Type::Program,
                            "The Godot editor this project opens its game in (Project panel › Start in Godot)."),
                    Scope::Project));

  // Level (LCFG chunk, undoable)
  p.declare(inScope(boolDef("Export/Collision", "Level/Godot Export", "Export collision", true,
                            "Off: the level's meshes get no static collision and clip brushes are left "
                            "out. Rigid bodies keep their hulls."),
                    Scope::Level));
  p.declare(inScope(boolDef("Export/Sun", "Level/Godot Export", "Export sun", true,
                            "A DirectionalLight3D named Sun: the level's sun (World Environment › Sun, or the "
                            "skybox's) with its direction, colour, energy and shadows."),
                    Scope::Level));
  p.declare(inScope(boolDef("Export/DevMaterials", "Level/Godot Export", "Export dev materials", false,
                            "On: faces with the dev texture (measurewall) are exported with it instead of left out."),
                    Scope::Level));

  // Asset server (local: net::LocalServer; remote: the URL and credentials below)
  {
    Def mode = choiceDef("Server/Mode", "Asset Server", "Asset server", {"Local", "Remote"},
                         {QStringLiteral("local"), QStringLiteral("remote")}, QStringLiteral("local"),
                         "Local: a server on this computer. Remote: the server below");
    mode.buttons = true;
    p.declare(mode);
  }
  {
    const auto remoteOnly = [](Def d) {
      d.enabledKey = QStringLiteral("Server/Mode");
      d.enabledValue = QStringLiteral("remote");
      return d;
    };
    p.declare(remoteOnly(pathDef("Server/Url", "Asset Server", "Server address",
                                 "", Type::String, "The remote asset server's URL, e.g. https://host/path or http://host:3000")));
    p.declare(remoteOnly(pathDef("Server/User", "Asset Server", "Username", "", Type::String,
                                 "Empty when the server needs no login")));
    Def pass = pathDef("Server/Password", "Asset Server", "Password", "", Type::String,
                       "Saved in settings.json");
    pass.secret = true;
    p.declare(remoteOnly(pass));
  }
  p.declare(pathDef("Server/DisplayName", "Asset Server", "Your name", "", Type::String,
                    "Shown to other editors connected to the same server (presence, edit locks). Empty = "
                    "user@host."));
  // External applications (default: GIMP found on PATH; a saved choice wins)
  {
    Def editor = pathDef("External/ImageEditor", "External applications", "Image editor", "", Type::Program,
                         "Program that opens images: texture maps (Edit map externally; saving the file uploads "
                         "it) and other images. Empty by default when GIMP is not on PATH.");
    editor.defaultValue = ui::external::detectImageEditor();
    p.declare(editor);
  }
  p.declare(pathDef("External/ImageEditorArgs", "External applications", "Image editor arguments", "{file}",
                    Type::String, "{file} is replaced by the path of the image."));
  // Migrate Textures/ImageEditor* once.
  for (const auto &[from, to]: {std::pair{"Textures/ImageEditor", "External/ImageEditor"},
                                std::pair{"Textures/ImageEditorArgs", "External/ImageEditorArgs"}}) {
    const QVariant old = config()->getRaw(QString::fromLatin1(from));
    if (!old.isValid())
      continue;
    config()->removeRaw(QString::fromLatin1(from));
    // "gimp" was a default: leave the PATH lookup in charge.
    if (old.toString().trimmed() != QLatin1String("gimp") && !old.toString().trimmed().isEmpty())
      p.setValue(QString::fromLatin1(to), old);
  }
  p.declare(intDef("Textures/CacheLimitMB", "Asset Server", "Download cache limit", 2048, 128, 65536, " MB",
                   "Thumbnails and 512 px editor maps kept on disk. The oldest files go first."));
  p.declare(intDef("Textures/BrowserPageSize", "Hidden", "Texture browser page size", 120, 30, 240));
  p.declare(intDef("Textures/BrowserThumbSize", "Hidden", "Texture browser card size", 256, 80, 256, " px"));
  p.declare(boolDef("Textures/RandomOrder", "Hidden", "Shuffle the texture lists", true));

  p.declare(pathDef("Paths/EntityDefinitions", "Entities", "Extra entity classes", "", Type::Path,
                    "A folder of .json entity classes, read at startup; one named like a built-in replaces it"));
  // Preferences › Paths (ui/paths_page.*) shows and sets it; empty = <config>/autosave.
  p.declare(pathDef("Paths/Autosave", "Hidden", "Autosave folder", "", Type::Path));

  p.declare(boolDef("Debug/Physics", "Debug", "Physics world (Box3D)", true,
                    "Keep a Box3D world of the map, rebuilt in the background after edits"));
  p.declare(boolDef("Debug/NoLights", "Debug", "Disable local lights", false));

  installRenderPresets(p);
}

// Quality presets: choosing one writes every value below; a hand edit switches to Custom. Nothing is applied at
// start-up, so saved custom values survive. The High column must match the declared defaults.
void installRenderPresets(Preferences &p) {
  struct Row {
    const char *key;
    QVariant low, medium, high, ultra;
  };
  static const Row rows[] = {
      {"Render/Msaa", 1, 2, 2, 4},
      {"Render/Fxaa", true, true, true, true},
      {"Render/MaxTextureSize", 1024, 2048, 4096, 8192},
      {"Render/ShadowMapSize", 1024, 2048, 4096, 4096},
      {"Render/ShadowCascades", 2, 3, 4, 4},
      {"Render/ShadowFilter", 1, 1, 1, 2},
      {"Render/ShadowTaps", 8, 16, 16, 32},
      {"Render/LocalShadowAtlas", 2048, 4096, 4096, 8192},
      {"Render/MaxShadowedLights", 2, 4, 8, 16},
      {"Render/Ssao", false, true, true, true},
      {"Render/SsaoQuality", 8, 8, 16, 32},
      {"Render/SsaoHalfRes", true, true, true, false},
  };
  static bool applying = false;
  QObject::connect(&p, &Preferences::changed, &p, [&p](const QString &key, const QVariant &value) {
    if (applying)
      return;
    const QString presetKey = QStringLiteral("Render/Preset");
    if (key == presetKey) {
      const int preset = value.toInt();
      if (preset < 0 || preset > 3)
        return;
      applying = true;
      for (const Row &r : rows) {
        const QVariant *col[] = {&r.low, &r.medium, &r.high, &r.ultra};
        p.setValue(QString::fromLatin1(r.key), *col[preset]);
      }
      applying = false;
      return;
    }
    for (const Row &r : rows) {
      if (key == QLatin1String(r.key)) {
        applying = true;
        p.setValue(presetKey, 4); // Custom
        applying = false;
        return;
      }
    }
  });
}

} // namespace prefs
