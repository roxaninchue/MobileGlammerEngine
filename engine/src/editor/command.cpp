#include "mge/editor/command.h"
#include "mge/log.h"

namespace mge::editor {

// ------------------------------------------------------------------ SetComponent
SetComponentCommand::SetComponentCommand(SceneDoc& doc, Uid uid, const serial::Registry::Type& type,
                                         std::vector<uint8_t> after, std::string label)
    : Command(CommandKind::SetComponent, label.empty() ? "Edit " + type.name : std::move(label)),
      uid_(uid), type_(&type), after_(std::move(after)) {
    before_ = doc.readComponent(uid, type);
    existed_ = !before_.empty();
}

bool SetComponentCommand::apply(SceneDoc& doc) { return doc.writeComponent(uid_, *type_, after_); }

void SetComponentCommand::revert(SceneDoc& doc) {
    if (existed_) doc.writeComponent(uid_, *type_, before_);
    else doc.removeComponent(uid_, *type_);
}

bool SetComponentCommand::mergeWith(const Command& next) {
    const auto& n = static_cast<const SetComponentCommand&>(next);   // kind checked by the stack
    if (n.uid_ != uid_ || n.type_ != type_) return false;
    after_ = n.after_;
    return true;
}

// ------------------------------------------------------------------ RemoveComponent
RemoveComponentCommand::RemoveComponentCommand(Uid uid, const serial::Registry::Type& type, std::string label)
    : Command(CommandKind::RemoveComponent, label.empty() ? "Remove " + type.name : std::move(label)),
      uid_(uid), type_(&type) {}

bool RemoveComponentCommand::apply(SceneDoc& doc) {
    before_ = doc.readComponent(uid_, *type_);
    return !before_.empty() && doc.removeComponent(uid_, *type_);
}

void RemoveComponentCommand::revert(SceneDoc& doc) { doc.writeComponent(uid_, *type_, before_); }

// ------------------------------------------------------------------ Spawn / Despawn
CommandPtr SpawnCommand::spawn(SubtreeSnapshot snapshot, std::string label) {
    auto* c = new SpawnCommand(CommandKind::Spawn, std::move(label));
    c->snapshot_ = std::move(snapshot);
    return CommandPtr(c);
}

CommandPtr SpawnCommand::despawn(std::vector<Uid> roots, std::string label) {
    auto* c = new SpawnCommand(CommandKind::Despawn, std::move(label));
    c->roots_ = std::move(roots);
    return CommandPtr(c);
}

bool SpawnCommand::apply(SceneDoc& doc) {
    if (kind() == CommandKind::Spawn) return doc.restore(snapshot_);
    // Despawn: capture on first apply, so the snapshot is exactly what was deleted (a redo
    // after later undos sees the same state again, since everything after it was undone).
    if (!roots_.empty()) {
        snapshot_ = doc.capture(roots_);
        roots_.clear();
        roots_.shrink_to_fit();
    }
    if (snapshot_.entities.empty()) return false;
    for (const auto& e : snapshot_.entities)
        if (!doc.entity(e.uid).valid()) return false;
    doc.destroy(snapshot_);
    return true;
}

void SpawnCommand::revert(SceneDoc& doc) {
    if (kind() == CommandKind::Spawn) doc.destroy(snapshot_);
    else doc.restore(snapshot_);
}

// ------------------------------------------------------------------ Group
bool GroupCommand::apply(SceneDoc& doc) {
    for (size_t i = 0; i < children_.size(); ++i) {
        if (children_[i]->apply(doc)) continue;
        while (i-- > 0) children_[i]->revert(doc);
        return false;
    }
    return true;
}

void GroupCommand::revert(SceneDoc& doc) {
    for (size_t i = children_.size(); i-- > 0;) children_[i]->revert(doc);
}

size_t GroupCommand::bytes() const {
    size_t b = sizeof(*this) + children_.capacity() * sizeof(CommandPtr);
    for (const auto& c : children_) b += c->bytes();
    return b;
}

// ------------------------------------------------------------------ CommandStack
bool CommandStack::execute(CommandPtr c) {
    if (!c) return false;
    if (locked_) {
        MGE_LOGW("editor: '%s' ignored, editing is locked while playing", c->label().c_str());
        return false;
    }
    if (c->kind() == CommandKind::SetComponent && static_cast<SetComponentCommand&>(*c).noop()) return true;
    if (!c->apply(doc_)) return false;
    ++revision_;
    if (!open_.empty()) {
        open_.back()->add(std::move(c));
        return true;
    }
    // Merge into the top step when it is the same continuous edit.
    if (!sealed_ && cursor_ == steps_.size() && cursor_ > 0 && c->mergeKey() != 0) {
        Step& top = steps_.back();
        if (top.cmd->kind() == c->kind() && top.cmd->mergeKey() == c->mergeKey()) {
            const size_t before = top.cmd->bytes();
            if (top.cmd->mergeWith(*c)) {
                bytes_ = bytes_ - before + top.cmd->bytes();
                if (top.serial == savedSerial_) top.serial = nextSerial_++;   // the saved state moved on
                return true;
            }
        }
    }
    record(std::move(c));
    return true;
}

void CommandStack::record(CommandPtr c) {
    // A new edit drops the redo branch (its serials never come back, so a save point there is lost).
    for (size_t i = cursor_; i < steps_.size(); ++i) bytes_ -= steps_[i].cmd->bytes();
    steps_.resize(cursor_);
    sealed_ = c->mergeKey() == 0;
    bytes_ += c->bytes();
    steps_.push_back({std::move(c), nextSerial_++});
    cursor_ = steps_.size();
    trim();
}

void CommandStack::trim() {
    size_t drop = 0;
    size_t bytes = bytes_;
    while (steps_.size() - drop > 1 && (steps_.size() - drop > cfg_.maxSteps || bytes > cfg_.maxBytes)) {
        bytes -= steps_[drop].cmd->bytes();
        ++drop;
    }
    if (!drop) return;
    baseSerial_ = steps_[drop - 1].serial;
    steps_.erase(steps_.begin(), steps_.begin() + ptrdiff_t(drop));
    cursor_ -= drop;
    bytes_ = bytes;
}

void CommandStack::begin(std::string label) {
    seal();
    open_.push_back(std::make_unique<GroupCommand>(std::move(label)));
}

void CommandStack::end() {
    MGE_ASSERT(!open_.empty(), "CommandStack::end without begin");
    if (open_.empty()) return;
    std::unique_ptr<GroupCommand> g = std::move(open_.back());
    open_.pop_back();
    if (g->empty()) return;
    if (!open_.empty()) { open_.back()->add(std::move(g)); return; }
    record(std::move(g));
    seal();
}

bool CommandStack::undo() {
    if (!canUndo()) return false;
    steps_[--cursor_].cmd->revert(doc_);
    seal();
    ++revision_;
    return true;
}

bool CommandStack::redo() {
    if (!canRedo()) return false;
    if (!steps_[cursor_].cmd->apply(doc_)) {
        MGE_LOGW("editor: redo of '%s' failed, history after it dropped", steps_[cursor_].cmd->label().c_str());
        for (size_t i = cursor_; i < steps_.size(); ++i) bytes_ -= steps_[i].cmd->bytes();
        steps_.resize(cursor_);
        ++revision_;
        return false;
    }
    ++cursor_;
    seal();
    ++revision_;
    return true;
}

void CommandStack::clear() {
    const bool wasClean = !isDirty();
    steps_.clear();
    open_.clear();
    cursor_ = 0;
    bytes_ = 0;
    baseSerial_ = nextSerial_++;
    savedSerial_ = wasClean ? baseSerial_ : 0;
    sealed_ = true;
    ++revision_;
}

void CommandStack::markSaved() {
    savedSerial_ = topSerial();
    ++revision_;
}

bool CommandStack::isDirty() const { return topSerial() != savedSerial_; }

}  // namespace mge::editor
