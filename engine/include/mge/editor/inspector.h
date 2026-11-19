#pragma once
// Editor inspector (M2 #22): any registered component, field by field, from the serialization
// registry (names, types, FieldHint). Values travel as text so the Compose panel and the AI
// layer can read and write them without knowing component layouts:
//   F32 "1.5"   I32/I16 "-3"   U32/U16 "7"   Vec2 "1.5 2"   Vec4 "0 0 1 1"
//   Angle hint: degrees "90"   Color hint: "#RRGGBBAA"   Bool hint: "true"/"false"
//   Str32: the text itself     Entity: editor id ("0" = none)
//   Enum hint: the option name ("Dynamic"); its index is accepted too
// Every write is a SetComponentCommand. Pass a non-zero `scrub` id while a value is being
// dragged: those writes merge into one undo step until the stack is sealed.
// Pure CPU code in mge_core, covered by host tests.
#include <string>
#include <string_view>
#include <vector>
#include "mge/editor/command.h"

namespace mge::editor {

struct InspectorField {
    std::string name;
    serial::FieldType type;
    serial::FieldHint hint;
    std::string value;
    std::vector<std::string> options;   // Enum hint only
};

// Names of an Enum field, in value order (empty for other fields).
std::vector<std::string> enumOptions(const serial::Field& f);

struct InspectorComponent {
    std::string type;
    bool removable = true;   // false for Transform2D on an entity that has children
    std::vector<InspectorField> fields;
};

class Inspector {
public:
    explicit Inspector(CommandStack& stack) : stack_(stack), doc_(stack.doc()) {}

    // Visible components of `uid` in registry order (hidden types and Name are left out:
    // the panel shows the name in its header).
    std::vector<InspectorComponent> inspect(Uid uid) const;
    // Registered, visible types `uid` does not have yet (the "Add component" menu).
    std::vector<std::string> addable(Uid uid) const;

    bool setField(Uid uid, std::string_view type, std::string_view field, std::string_view value, uint64_t scrub = 0);
    bool addComponent(Uid uid, std::string_view type);
    bool removeComponent(Uid uid, std::string_view type);

    // Text <-> field bytes (exposed for tests and tools). parse returns false on bad input.
    static std::string format(const serial::Field& f, const uint8_t* bytes);
    static bool parse(const serial::Field& f, std::string_view text, uint8_t* bytes);

private:
    CommandStack& stack_;
    SceneDoc& doc_;
};

}  // namespace mge::editor
