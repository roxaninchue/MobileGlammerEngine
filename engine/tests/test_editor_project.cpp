#include "test_main.h"
#include "mge/editor/editor_session.h"
#include "mge/editor/project.h"
#include <cstdlib>
#include <string>
#include <unistd.h>
using namespace mge;
using namespace mge::editor;

namespace {
// Fresh empty folder per test under the system temp dir.
std::string tempRoot() {
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base && *base ? base : "/tmp") + "/mge_projects_XXXXXX";
    char* made = ::mkdtemp(tmpl.data());
    return made ? std::string(made) + "/projects" : std::string();
}
Uid byName(EditorSession& s, const char* name) {
    for (const TreeRow& r : s.hierarchy().rows(s.selection())) if (r.name == name) return r.uid;
    return kNoUid;
}
bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }
int64_t fakeClock = 1000;
int64_t tick() { return ++fakeClock; }
}  // namespace

TEST(project_names_and_slugs) {
    CHECK(ProjectStore::slugFor("  My Game! ") == "my-game");
    CHECK(ProjectStore::slugFor("Space  -- Race 2") == "space-race-2");
    CHECK(ProjectStore::slugFor("élan") == "lan");
    CHECK(ProjectStore::validName("Hat \"red\""));
    CHECK(!ProjectStore::validName("   "));
    CHECK(!ProjectStore::validName("!!!"));            // nothing left for a folder name
    CHECK(!ProjectStore::validName("a\tb"));
    CHECK(!ProjectStore::validName(std::string(49, 'x')));
    CHECK(ProjectStore::validName(std::string(48, 'x')));
}

TEST(project_store_save_load_list_rename_remove) {
    ProjectStore store(tempRoot());
    store.now = tick;
    CHECK(store.ok() && store.list().empty() && store.last().empty());
    CHECK(store.save("Alpha", "{\"a\":1}"));
    CHECK(store.save("Beta \"two\"", "{\"b\":2}"));
    std::string json;
    CHECK(store.load("alpha", json) && json == "{\"a\":1}");   // names match by slug
    std::vector<ProjectInfo> l = store.list();
    CHECK(l.size() == 2 && l[0].name == "Beta \"two\"" && l[0].slug == "beta-two" && l[1].name == "Alpha");
    CHECK(l[0].modified > l[1].modified);
    CHECK(store.uniqueName("Alpha") == "Alpha 2" && store.uniqueName("Gamma") == "Gamma");
    // Overwrite keeps one project and moves it to the top.
    CHECK(store.save("Alpha", "{\"a\":3}"));
    l = store.list();
    CHECK(l.size() == 2 && l[0].name == "Alpha" && store.load("Alpha", json) && json == "{\"a\":3}");
    // No temp files left behind.
    CHECK(::access((store.root() + "/alpha/main.scene.json.tmp").c_str(), F_OK) != 0);
    // Rename (folder and display name); refuses a name another project has.
    store.setLast("Alpha");
    CHECK(!store.rename("Alpha", "beta two"));
    CHECK(store.rename("Alpha", "Alpha Prime"));
    CHECK(!store.exists("Alpha") && store.load("Alpha Prime", json) && json == "{\"a\":3}");
    CHECK(store.last() == "Alpha Prime");
    // Remove.
    CHECK(store.remove("Alpha Prime") && !store.exists("Alpha Prime") && store.last().empty());
    CHECK(!store.remove("Alpha Prime"));
    CHECK(store.list().size() == 1);
}

TEST(session_first_launch_creates_and_reopens_last_project) {
    const std::string root = tempRoot();
    std::string sceneBefore;
    {
        EditorSession s;
        CHECK(s.openProjects(root));
        CHECK(s.projectName() == "My Game" && !s.stack().isDirty());
        CHECK(byName(s, "Crate") != kNoUid);                  // starter scene
        const Uid crate = byName(s, "Crate");
        CHECK(s.handle("rename " + std::to_string(crate) + " Barrel"));
        CHECK(s.stack().isDirty() && has(s.stateJson(), "\"dirty\":true"));
        CHECK(s.handle("save"));
        CHECK(!s.stack().isDirty() && s.stack().canUndo());   // saving keeps the history
        CHECK(has(s.stateJson(), "\"project\":{\"name\":\"My Game\",\"list\":[{\"name\":\"My Game\",\"modified\":"));
        sceneBefore = s.sceneJson();
    }
    EditorSession s2;
    CHECK(s2.openProjects(root));
    CHECK(s2.projectName() == "My Game" && byName(s2, "Barrel") != kNoUid && byName(s2, "Crate") == kNoUid);
    CHECK(!s2.stack().isDirty() && !s2.stack().canUndo());
    CHECK(s2.sceneJson() == sceneBefore);                    // same ids, same values
    // Editor ids carry on after the loaded ones.
    CHECK(s2.handle("create 0 Fresh"));
    CHECK(byName(s2, "Fresh") > byName(s2, "Barrel"));
}

TEST(session_saveas_new_open_rename_delete) {
    EditorSession s;
    CHECK(s.openProjects(tempRoot()));
    CHECK(s.handle("saveas Level Two"));
    CHECK(s.projectName() == "Level Two" && s.projects()->list().size() == 2);
    CHECK(!s.handle("saveas my game"));   // another project: never overwritten
    CHECK(s.handle("newproject Empty"));
    CHECK(s.projectName() == "Empty" && s.hierarchy().rows(s.selection()).empty());
    CHECK(s.handle("newproject Empty"));  // taken: gets a fresh name
    CHECK(s.projectName() == "Empty 2");
    CHECK(s.handle("open Level Two") && s.projectName() == "Level Two" && byName(s, "Player") != kNoUid);
    CHECK(!s.handle("open Nowhere") && s.projectName() == "Level Two");
    CHECK(!s.handle("deleteproject Level Two"));   // the open one
    CHECK(s.handle("deleteproject Empty 2") && !s.projects()->exists("Empty 2"));
    CHECK(s.handle("renameproject Level 2") && s.projectName() == "Level 2" && s.projects()->exists("Level 2"));
    CHECK(s.projects()->last() == "Level 2");
    CHECK(!s.handle("renameproject Empty"));        // taken
    CHECK(!s.handle("saveas    "));
}

TEST(session_save_while_playing_saves_the_scene_from_before_play) {
    EditorSession s;
    CHECK(s.openProjects(tempRoot()));
    const std::string before = s.sceneJson();
    CHECK(s.handle("play"));
    for (int i = 0; i < 60; ++i) { s.tick(1.0f / 60.0f); s.update(); }
    CHECK(serial::saveJson(s.world(), s.registry()) != before);   // the game moved things
    CHECK(s.sceneJson() == before);
    CHECK(s.handle("save") && s.play().playing());
    std::string onDisk;
    CHECK(s.projects()->load("My Game", onDisk) && onDisk == before);
    // Opening another project ends play.
    CHECK(s.handle("saveas Copy") && s.handle("open My Game"));
    CHECK(!s.play().active() && !s.stack().locked());
}

TEST(session_autosave_and_bad_scene_files) {
    const std::string root = tempRoot();
    EditorSession s;
    CHECK(s.openProjects(root));
    CHECK(s.autosave());                           // clean: nothing to write
    const Uid player = byName(s, "Player");
    CHECK(s.handle("set " + std::to_string(player) + " Transform2D position 4 5"));
    CHECK(s.autosave() && !s.stack().isDirty());
    // A torn or foreign file is refused and the open scene stays.
    ProjectStore other(root);
    CHECK(other.save("Broken", "{ not a scene"));
    const std::string open = s.sceneJson();
    CHECK(!s.handle("open Broken"));
    CHECK(s.projectName() == "My Game" && s.sceneJson() == open);
    CHECK(!s.loadSceneJson("[]"));
    // Without a store, project commands are refused.
    EditorSession none;
    none.newScene(true);
    CHECK(!none.handle("save") && !none.handle("saveas X") && !none.autosave());
    CHECK(has(none.stateJson(), "\"project\":{\"name\":\"\",\"list\":[]}"));
}
