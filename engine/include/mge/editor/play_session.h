#pragma once
// Play-in-editor (M2 #24): run the scene being edited, then put it back exactly.
//  - play(): snapshots the scene (binary, every registered component, editor ids included),
//    locks the CommandStack (no edits, no undo while playing), builds the game systems
//    (physics bodies from Collider2D) and starts the clock. From Paused it just resumes.
//  - pause() / step(): freeze the simulation; step advances exactly one fixed physics step.
//  - stop(): throws the played world away and reloads the snapshot into the same ECS world:
//    same ids, same values bit for bit, selection kept, undo history intact.
//  - The editor renders on demand while editing; the host keeps frames coming while
//    playing() is true.
// Pure CPU code in mge_core, covered by host tests.
#include <cstdint>
#include <memory>
#include <vector>
#include "mge/physics2d/physics_system.h"

namespace mge::editor {

class EditorSession;

enum class PlayState : uint8_t { Editing, Playing, Paused };

class PlaySession {
public:
    PlaySession();
    ~PlaySession();

    bool play(EditorSession& session);
    bool pause();
    bool step(EditorSession& session);
    bool stop(EditorSession& session);

    // Advances the game by `dt` seconds while Playing (no-op otherwise). Returns true when the
    // scene changed.
    bool update(EditorSession& session, float dt);

    PlayState state() const { return state_; }
    bool playing() const { return state_ == PlayState::Playing; }
    bool active() const { return state_ != PlayState::Editing; }
    float time() const { return time_; }          // simulated seconds since play()
    uint32_t steps() const { return steps_; }     // fixed physics steps since play()
    physics2d::PhysicsSystem2D* physics() { return physics_.get(); }
    size_t snapshotBytes() const { return snapshot_.size(); }
    // The scene as it was at play() (binary); empty while editing.
    const std::vector<uint8_t>& snapshot() const { return snapshot_; }

    physics2d::WorldDef worldDef;   // gravity etc. for the next play()

private:
    PlayState state_ = PlayState::Editing;
    std::vector<uint8_t> snapshot_;
    std::unique_ptr<physics2d::PhysicsSystem2D> physics_;
    float time_ = 0;
    uint32_t steps_ = 0;
};

}  // namespace mge::editor
