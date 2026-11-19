#pragma once
// Projects on disk (M2 #25). A project is a folder under the store's root:
//     <root>/<slug>/project.json      {"format":1,"name":"My Game","scene":"main.scene.json","modified":1763600000}
//     <root>/<slug>/main.scene.json   the scene (serial::saveJson: readable, diffable, and what the
//                                     AI layer will edit in M4)
//     <root>/last                     name of the project to reopen on launch
// The slug is the name lowercased with anything but [a-z0-9] turned into '-' ("My Game!" ->
// "my-game"), so two names that slug the same are the same project. Every write goes to a
// temporary file first and is renamed over the old one, so a crash mid-save never leaves a torn
// scene. POSIX only (Linux hosts and Android). Pure CPU code in mge_core, covered by host tests.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mge::editor {

struct ProjectInfo {
    std::string name;       // display name, as typed
    std::string slug;       // folder name
    int64_t modified = 0;   // unix seconds of the last save
};

class ProjectStore {
public:
    static constexpr size_t kMaxName = 48;
    static constexpr const char* kSceneFile = "main.scene.json";

    // Creates `root` (and its parents) when missing.
    explicit ProjectStore(std::string root);

    const std::string& root() const { return root_; }
    bool ok() const { return ok_; }   // root exists and is a folder

    // Newest first (then by name).
    std::vector<ProjectInfo> list() const;
    bool exists(std::string_view name) const;

    // Creates or overwrites the project's scene and stamps it modified now.
    bool save(std::string_view name, std::string_view sceneJson);
    bool load(std::string_view name, std::string& sceneJson) const;
    bool remove(std::string_view name);
    // Renames a project (folder and display name). Fails when `to` is taken by another project.
    bool rename(std::string_view from, std::string_view to);

    // The project to reopen on launch ("" when none was recorded).
    std::string last() const;
    void setLast(std::string_view name);

    // 1..kMaxName characters after trimming, no control characters, and a non-empty slug.
    static bool validName(std::string_view name);
    static std::string trimName(std::string_view name);
    static std::string slugFor(std::string_view name);
    // "My Game" -> "My Game 2" -> "My Game 3"... the first name not taken in this store.
    std::string uniqueName(std::string_view base) const;

    // Clock for "modified" stamps; tests pin it.
    int64_t (*now)() = nullptr;

private:
    std::string dirOf(std::string_view name) const { return root_ + "/" + slugFor(name); }
    bool readInfo(const std::string& slug, ProjectInfo& out) const;
    bool writeInfo(const std::string& dir, std::string_view name) const;

    std::string root_;
    bool ok_ = false;
};

// Whole-file helpers (binary safe). writeFileAtomic writes `path`.tmp then renames it over `path`.
bool readFile(const std::string& path, std::string& out);
bool writeFileAtomic(const std::string& path, std::string_view data);

}  // namespace mge::editor
