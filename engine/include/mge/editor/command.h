#pragma once
// Editor undo/redo (M2 #21). Every edit the editor makes is a Command pushed on a CommandStack.
//  - Commands name entities by editor id (SceneDoc), so they stay valid across delete/undo.
//  - Continuous edits merge: a gizmo drag or a slider scrub pushes one SetComponent per frame
//    with the same merge key, and they fold into a single undo step until the stack is sealed
//    (finger lifted) or something else is pushed.
//  - Transactions group several commands into one undo step ("Duplicate 3 entities").
//  - History is capped by step count and by bytes; the oldest steps are dropped first.
//  - Save point: isDirty() says whether the scene differs from the last save, including after
//    undoing past it or redoing back to it.
//  - lock(): while playing in the editor the stack refuses new commands, undo and redo.
// Pure CPU code in mge_core, covered by host tests.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "mge/editor/scene_doc.h"

namespace mge::editor {

enum class CommandKind : uint8_t { SetComponent, AddComponent, RemoveComponent, Spawn, Despawn, Group, Custom };

class Command {
public:
    virtual ~Command() = default;
    // Do / redo. Returns false when it could not be applied (nothing must have changed then).
    virtual bool apply(SceneDoc& doc) = 0;
    virtual void revert(SceneDoc& doc) = 0;
    // Folds `next` (same kind and merge key, already applied) into this one. Default: no merging.
    virtual bool mergeWith(const Command& /*next*/) { return false; }
    virtual size_t bytes() const { return sizeof(*this); }

    CommandKind kind() const { return kind_; }
    uint64_t mergeKey() const { return mergeKey_; }   // 0 = never merges
    const std::string& label() const { return label_; }
    void setLabel(std::string l) { label_ = std::move(l); }
    void setMergeKey(uint64_t k) { mergeKey_ = k; }

protected:
    Command(CommandKind kind, std::string label) : kind_(kind), label_(std::move(label)) {}

private:
    CommandKind kind_;
    uint64_t mergeKey_ = 0;
    std::string label_;
};

using CommandPtr = std::unique_ptr<Command>;

// Writes a whole registry component (adds it if missing). Undo writes the old bytes back, or
// removes the component when it did not exist before.
class SetComponentCommand final : public Command {
public:
    // `after` uses the SceneDoc encoding (entity refs as ids); readComponent() gives one to edit.
    SetComponentCommand(SceneDoc& doc, Uid uid, const serial::Registry::Type& type, std::vector<uint8_t> after,
                        std::string label = {});
    // With an explicit `before` (the component existed): for edits previewed live before they
    // are recorded, like a gizmo drag.
    SetComponentCommand(Uid uid, const serial::Registry::Type& type, std::vector<uint8_t> before,
                        std::vector<uint8_t> after, std::string label);
    bool apply(SceneDoc& doc) override;
    void revert(SceneDoc& doc) override;
    bool mergeWith(const Command& next) override;
    size_t bytes() const override { return sizeof(*this) + before_.capacity() + after_.capacity(); }

    Uid uid() const { return uid_; }
    const serial::Registry::Type& type() const { return *type_; }
    const std::vector<uint8_t>& after() const { return after_; }
    bool noop() const { return existed_ && before_ == after_; }

private:
    Uid uid_;
    const serial::Registry::Type* type_;
    std::vector<uint8_t> before_, after_;
    bool existed_;
};

// Removes a registry component; undo puts the same bytes back.
class RemoveComponentCommand final : public Command {
public:
    RemoveComponentCommand(Uid uid, const serial::Registry::Type& type, std::string label = {});
    bool apply(SceneDoc& doc) override;
    void revert(SceneDoc& doc) override;
    size_t bytes() const override { return sizeof(*this) + before_.capacity(); }

private:
    Uid uid_;
    const serial::Registry::Type* type_;
    std::vector<uint8_t> before_;
};

// Spawn: brings a snapshot to life (create entities, paste, duplicate, undo of delete).
// Despawn: captures the subtrees on first apply and destroys them (delete).
class SpawnCommand final : public Command {
public:
    static CommandPtr spawn(SubtreeSnapshot snapshot, std::string label = "Create");
    static CommandPtr despawn(std::vector<Uid> roots, std::string label = "Delete");
    bool apply(SceneDoc& doc) override;
    void revert(SceneDoc& doc) override;
    size_t bytes() const override { return sizeof(*this) + snapshot_.bytes() + roots_.capacity() * sizeof(Uid); }
    const SubtreeSnapshot& snapshot() const { return snapshot_; }

private:
    SpawnCommand(CommandKind k, std::string label) : Command(k, std::move(label)) {}
    SubtreeSnapshot snapshot_;
    std::vector<Uid> roots_;   // despawn only, until the first capture
};

// Several commands as one undo step; reverted in reverse order.
class GroupCommand final : public Command {
public:
    explicit GroupCommand(std::string label) : Command(CommandKind::Group, std::move(label)) {}
    void add(CommandPtr c) { children_.push_back(std::move(c)); }
    bool apply(SceneDoc& doc) override;
    void revert(SceneDoc& doc) override;
    size_t bytes() const override;
    bool empty() const { return children_.empty(); }
    size_t size() const { return children_.size(); }
    const Command& child(size_t i) const { return *children_[i]; }

private:
    std::vector<CommandPtr> children_;
};

struct CommandStackConfig {
    uint32_t maxSteps = 256;
    size_t maxBytes = 8u << 20;   // 8 MiB of history
};

class CommandStack {
public:
    explicit CommandStack(SceneDoc& doc, const CommandStackConfig& cfg = {}) : doc_(doc), cfg_(cfg) {}

    // Applies `c` and records it (or folds it into the open transaction / the mergeable top).
    // Returns false (and records nothing) if the stack is locked or the command failed.
    bool execute(CommandPtr c);
    // Ends the current merge run: the next command starts a new undo step even with the same key.
    void seal() { sealed_ = true; }

    // Transactions nest; only the outermost end() records the group (empty groups vanish).
    void begin(std::string label);
    void end();
    bool inTransaction() const { return !open_.empty(); }

    bool undo();
    bool redo();
    bool canUndo() const { return !locked_ && open_.empty() && cursor_ > 0; }
    bool canRedo() const { return !locked_ && open_.empty() && cursor_ < steps_.size(); }
    std::string undoLabel() const { return cursor_ > 0 ? steps_[cursor_ - 1].cmd->label() : std::string(); }
    std::string redoLabel() const { return cursor_ < steps_.size() ? steps_[cursor_].cmd->label() : std::string(); }
    void clear();

    void markSaved();
    bool isDirty() const;

    void lock(bool locked) { locked_ = locked; seal(); }
    bool locked() const { return locked_; }

    size_t size() const { return steps_.size(); }
    size_t cursor() const { return cursor_; }
    size_t historyBytes() const { return bytes_; }
    uint64_t revision() const { return revision_; }
    SceneDoc& doc() { return doc_; }

private:
    struct Step { CommandPtr cmd; uint64_t serial; };
    void record(CommandPtr c);
    uint64_t topSerial() const { return cursor_ ? steps_[cursor_ - 1].serial : baseSerial_; }
    void trim();

    SceneDoc& doc_;
    CommandStackConfig cfg_;
    std::vector<Step> steps_;
    size_t cursor_ = 0;          // steps_[0, cursor_) are applied
    size_t bytes_ = 0;
    std::vector<std::unique_ptr<GroupCommand>> open_;
    bool sealed_ = true;
    bool locked_ = false;
    uint64_t nextSerial_ = 1;
    uint64_t baseSerial_ = 0;    // state with no step applied: serial of the last step trimmed away
    uint64_t savedSerial_ = 0;   // serial of the state at save time (top applied step, or base)
    uint64_t revision_ = 0;
};

}  // namespace mge::editor
