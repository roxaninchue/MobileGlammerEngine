#pragma once
// Scene serialization.
//  - Binary: compact, for shipping games and fast load. Carries its own schema (component and
//    field names + types), so old saves load after components gain, lose or reorder fields:
//    fields are matched by name, missing ones keep their defaults, unknown ones are skipped.
//  - JSON: same data, human/AI readable and editable. Entities may carry any integer "id";
//    entity references point at those ids.
//  - Loads are all-or-nothing: on any error the entities created so far are destroyed.
//  - Derived components (e.g. WorldTransform2D) are not saved; onAdd hooks rebuild them.
// Binary is little-endian (every Android ABI we target).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>
#include "mge/ecs.h"

namespace mge::serial {

constexpr uint32_t kFormatVersion = 1;

enum class FieldType : uint8_t { F32, I32, U32, Vec2, Entity };

struct Field {
    const char* name;
    FieldType type;
    uint32_t offset;
};
#define MGE_FIELD(T, member, ftype) \
    ::mge::serial::Field{#member, ::mge::serial::FieldType::ftype, uint32_t(offsetof(T, member))}

class Registry {
public:
    struct Type {
        std::string name;
        ecs::ComponentId id;
        uint32_t size;
        std::vector<Field> fields;
        std::vector<uint8_t> defaults;                          // bytes of T{}
        void (*onAdd)(ecs::World&, ecs::Entity) = nullptr;      // runs after a load adds it
    };

    template <class T> Type& add(const char* name, std::initializer_list<Field> fields) {
        Type t;
        t.name = name;
        t.id = ecs::componentId<T>();
        t.size = sizeof(T);
        t.fields.assign(fields);
        t.defaults.resize(sizeof(T));
        T def{};
        std::memcpy(t.defaults.data(), &def, sizeof(T));
        types_.push_back(std::move(t));
        return types_.back();
    }
    const Type* find(std::string_view name) const;
    const std::vector<Type>& types() const { return types_; }

private:
    std::vector<Type> types_;
};

// Transform2D (+ rebuilds WorldTransform2D) and Parent.
void registerScene2D(Registry& reg);

std::vector<uint8_t> saveBinary(ecs::World& world, const Registry& reg);
bool loadBinary(ecs::World& world, const Registry& reg, const uint8_t* data, size_t size,
                std::vector<ecs::Entity>* created = nullptr);

std::string saveJson(ecs::World& world, const Registry& reg);
bool loadJson(ecs::World& world, const Registry& reg, std::string_view json,
              std::vector<ecs::Entity>* created = nullptr);

}  // namespace mge::serial
