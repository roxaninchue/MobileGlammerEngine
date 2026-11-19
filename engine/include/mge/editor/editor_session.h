#pragma once
// Editor session (M2 #22): one edited scene with everything the editor UI talks to.
//  - Owns the registry (scene2d + render2d + editor types), the ECS world, the transform
//    system, the SceneDoc, the CommandStack, the hierarchy/inspector models and the selection.
//  - handle(line): the text protocol the Compose panels (and later the AI layer) send over JNI.
//    One command per call; ids are editor ids; the last argument takes the rest of the line:
//        select <id>            toggle <id>           expand <id> <0|1>
//        create <parent> [name] rename <id> <name>    duplicate             delete
//        reparent <id> <parent> <index>               reorder <id> <index>
//        set <id> <Type> <field> <value>              scrub <key> <id> <Type> <field> <value>
//        seal                   add <id> <Type>       remove <id> <Type>    undo     redo
//        gizmo <move|rotate|scale>                    space <world|local>
//        snap <0|1> [grid] [angleDeg] [scaleStep]
//        play                   pause                 step                  stop
//    Parent 0 means the scene root. Returns false for unknown or rejected commands.
//  - stateJson(): what the panels show (tree rows, inspector of the primary selection,
//    undo/redo labels, dirty flag), stamped with revision() so the UI re-reads only on change.
// Pure CPU code in mge_core, covered by host tests.
#include <memory>
#include <string>
#include <string_view>
#include "mge/editor/command.h"
#include "mge/editor/gizmo.h"
#include "mge/editor/hierarchy.h"
#include "mge/editor/inspector.h"
#include "mge/editor/play_session.h"
#include "mge/render/sprite_batch.h"
#include "mge/render/sprite_system.h"
#include "mge/serialize.h"
#include "mge/transform2d.h"

namespace mge::editor {

class EditorSession {
public:
    EditorSession();
    ~EditorSession();

    // Empties the scene and the history; `starter` adds a small sample scene.
    void newScene(bool starter = true);

    bool handle(std::string_view line);
    std::string stateJson();
    uint64_t revision() const { return doc_->revision() + stack_->revision() + selRev_; }

    // Topmost visible sprite under a world point (highest layer, then latest in hierarchy).
    Uid pick(vec2 world);
    void select(Uid uid, bool toggle = false);

    // Advances play mode by `dt` (no-op while editing); true when the scene changed.
    bool tick(float dt);
    // Resolves world transforms; call once per frame before draw().
    void update();
    // Scene sprites, plus the editor overlay (grid, selection outlines, gizmo) sized for the
    // view's pixels per world unit. Between batcher.begin() and end().
    void draw(render::SpriteBatcher& batcher, const render::ScreenLayout& view, bool overlay = true);

    serial::Registry& registry() { return reg_; }
    ecs::World& world() { return *world_; }
    scene2d::TransformSystem2D& transforms() { return *transforms_; }
    SceneDoc& doc() { return *doc_; }
    CommandStack& stack() { return *stack_; }
    Hierarchy& hierarchy() { return *hierarchy_; }
    Inspector& inspector() { return *inspector_; }
    Selection& selection() { return selection_; }
    Gizmo& gizmo() { return gizmo_; }
    PlaySession& play() { return play_; }

    // Grid spacing in world units for the overlay (and snapping, later).
    float gridStep = 1.0f;

private:
    void rebuild();   // fresh world + doc + stack over the same registry
    void buildStarter();

    serial::Registry reg_;
    std::unique_ptr<ecs::World> world_;
    std::unique_ptr<scene2d::TransformSystem2D> transforms_;
    std::unique_ptr<SceneDoc> doc_;
    std::unique_ptr<CommandStack> stack_;
    std::unique_ptr<Hierarchy> hierarchy_;
    std::unique_ptr<Inspector> inspector_;
    Selection selection_;
    Gizmo gizmo_;
    PlaySession play_;
    int playTick_ = -1;   // last quarter second of play time the UI was told about
    uint64_t selRev_ = 0;   // selection changes; rebuild() seeds it so revision() keeps increasing
};

// JSON string literal (quotes included) for UI payloads.
void appendJsonString(std::string& out, std::string_view s);

}  // namespace mge::editor
