#include "mge/editor/play_session.h"
#include "mge/editor/editor_session.h"
#include "mge/log.h"

namespace mge::editor {

PlaySession::PlaySession() = default;
PlaySession::~PlaySession() = default;

bool PlaySession::play(EditorSession& session) {
    if (state_ == PlayState::Playing) return false;
    if (state_ == PlayState::Paused) {
        state_ = PlayState::Playing;
        if (physics_) physics_->interpolate = true;
        return true;
    }
    session.gizmo().cancel(session);   // a drag in flight is not part of the scene to play
    session.update();                  // world transforms current: colliders read them
    snapshot_ = serial::saveBinary(session.world(), session.registry());
    session.stack().lock(true);
    physics_ = std::make_unique<physics2d::PhysicsSystem2D>(worldDef);
    const uint32_t bodies = physics_->createFromColliders(session.world());
    time_ = 0;
    steps_ = 0;
    state_ = PlayState::Playing;
    MGE_LOGI("editor: play (%u bodies, snapshot %zu bytes)", bodies, snapshot_.size());
    return true;
}

bool PlaySession::pause() {
    if (state_ != PlayState::Playing) return false;
    state_ = PlayState::Paused;
    if (physics_) physics_->interpolate = false;   // show exactly the last step
    return true;
}

bool PlaySession::step(EditorSession& session) {
    if (state_ != PlayState::Paused || !physics_) return false;
    // The accumulator holds less than one step, so adding exactly one step runs exactly one.
    const float dt = physics_->world().fixedDt();
    physics_->update(session.world(), dt);
    steps_ += uint32_t(physics_->world().stats().stepsLastAdvance);
    time_ += dt;
    return true;
}

bool PlaySession::update(EditorSession& session, float dt) {
    if (state_ != PlayState::Playing || !physics_) return false;
    physics_->update(session.world(), dt);
    steps_ += uint32_t(physics_->world().stats().stepsLastAdvance);
    time_ += dt;
    return true;
}

bool PlaySession::stop(EditorSession& session) {
    if (state_ == PlayState::Editing) return false;
    physics_.reset();
    // Back to the snapshot in the same ECS world: everything the game created or changed goes.
    ecs::World& w = session.world();
    std::vector<ecs::Entity> all;
    w.eachEntity([&](ecs::Entity e) { all.push_back(e); });
    for (ecs::Entity e : all) w.destroy(e);
    const bool ok = serial::loadBinary(w, session.registry(), snapshot_.data(), snapshot_.size());
    if (!ok) MGE_LOGE("editor: restoring the scene after play failed (snapshot %zu bytes)", snapshot_.size());
    session.doc().adopt();   // same ids as before play: the selection and the history still apply
    session.doc().touch();
    snapshot_.clear();
    snapshot_.shrink_to_fit();
    session.stack().lock(false);
    state_ = PlayState::Editing;
    MGE_LOGI("editor: stop after %.2f s (%u steps)", double(time_), steps_);
    return ok;
}

}  // namespace mge::editor
