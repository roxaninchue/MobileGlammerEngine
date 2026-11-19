#include "mge/editor/project.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mge/editor/editor_session.h"   // appendJsonString
#include "mge/log.h"

namespace mge::editor {
namespace {

bool isDir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool makeDirs(const std::string& path) {
    if (path.empty() || isDir(path)) return !path.empty();
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        const std::string part = path.substr(0, i);
        if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return isDir(path);
}

bool removeTree(const std::string& path) {
    if (DIR* d = ::opendir(path.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string child = path + "/" + n;
            if (isDir(child)) removeTree(child);
            else ::unlink(child.c_str());
        }
        ::closedir(d);
    }
    return ::rmdir(path.c_str()) == 0;
}

int64_t systemNow() { return int64_t(std::time(nullptr)); }

// Reads the string value of "key" in a flat JSON object (enough for project.json).
bool jsonString(std::string_view json, std::string_view key, std::string& out) {
    const std::string pat = "\"" + std::string(key) + "\"";
    size_t i = json.find(pat);
    if (i == std::string_view::npos) return false;
    i = json.find(':', i + pat.size());
    if (i == std::string_view::npos) return false;
    i = json.find('"', i);
    if (i == std::string_view::npos) return false;
    out.clear();
    for (++i; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (++i >= json.size()) return false;
        c = json[i];
        switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 >= json.size()) return false;
                const unsigned v = unsigned(std::strtoul(std::string(json.substr(i + 1, 4)).c_str(), nullptr, 16));
                i += 4;
                if (v < 0x80) out += char(v);
                else if (v < 0x800) { out += char(0xC0 | (v >> 6)); out += char(0x80 | (v & 0x3F)); }
                else { out += char(0xE0 | (v >> 12)); out += char(0x80 | ((v >> 6) & 0x3F)); out += char(0x80 | (v & 0x3F)); }
                break;
            }
            default: out += c;   // \" \\ \/
        }
    }
    return false;
}

bool jsonInt(std::string_view json, std::string_view key, int64_t& out) {
    const std::string pat = "\"" + std::string(key) + "\"";
    size_t i = json.find(pat);
    if (i == std::string_view::npos) return false;
    i = json.find(':', i + pat.size());
    if (i == std::string_view::npos) return false;
    out = std::strtoll(std::string(json.substr(i + 1, 24)).c_str(), nullptr, 10);
    return true;
}

}  // namespace

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

bool writeFileAtomic(const std::string& path, std::string_view data) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fflush(f) == 0 && ok;
    ok = ::fsync(::fileno(f)) == 0 && ok;
    ok = std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

ProjectStore::ProjectStore(std::string root) : root_(std::move(root)) {
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    ok_ = makeDirs(root_);
    if (!ok_) MGE_LOGE("projects: cannot create '%s'", root_.c_str());
}

std::string ProjectStore::trimName(std::string_view name) {
    auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!name.empty() && blank(name.front())) name.remove_prefix(1);
    while (!name.empty() && blank(name.back())) name.remove_suffix(1);
    return std::string(name);
}

std::string ProjectStore::slugFor(std::string_view name) {
    const std::string t = trimName(name);
    std::string s;
    for (char c : t) {
        const char l = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        const bool keep = (l >= 'a' && l <= 'z') || (l >= '0' && l <= '9');
        if (keep) s += l;
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s;
}

bool ProjectStore::validName(std::string_view name) {
    const std::string t = trimName(name);
    if (t.empty() || t.size() > kMaxName) return false;
    for (char c : t) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
    return !slugFor(t).empty();
}

bool ProjectStore::readInfo(const std::string& slug, ProjectInfo& out) const {
    std::string json;
    if (!readFile(root_ + "/" + slug + "/project.json", json)) return false;
    out = {};
    out.slug = slug;
    if (!jsonString(json, "name", out.name) || out.name.empty()) out.name = slug;
    jsonInt(json, "modified", out.modified);
    return true;
}

bool ProjectStore::writeInfo(const std::string& dir, std::string_view name) const {
    std::string json = "{\"format\":1,\"name\":";
    appendJsonString(json, trimName(name));
    json += ",\"scene\":\"";
    json += kSceneFile;
    json += "\",\"modified\":" + std::to_string(now ? now() : systemNow()) + "}\n";
    return writeFileAtomic(dir + "/project.json", json);
}

std::vector<ProjectInfo> ProjectStore::list() const {
    std::vector<ProjectInfo> out;
    DIR* d = ::opendir(root_.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        ProjectInfo info;
        if (isDir(root_ + "/" + n) && readInfo(n, info)) out.push_back(std::move(info));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const ProjectInfo& a, const ProjectInfo& b) {
        return a.modified != b.modified ? a.modified > b.modified : a.name < b.name;
    });
    return out;
}

bool ProjectStore::exists(std::string_view name) const {
    ProjectInfo info;
    return validName(name) && readInfo(slugFor(name), info);
}

bool ProjectStore::save(std::string_view name, std::string_view sceneJson) {
    if (!ok_ || !validName(name)) return false;
    const std::string dir = dirOf(name);
    if (!makeDirs(dir)) return false;
    // Scene first, then the info stamp: a project.json always points at a complete scene.
    if (!writeFileAtomic(dir + "/" + kSceneFile, sceneJson) || !writeInfo(dir, name)) {
        MGE_LOGE("projects: saving '%s' failed", dir.c_str());
        return false;
    }
    return true;
}

bool ProjectStore::load(std::string_view name, std::string& sceneJson) const {
    return validName(name) && readFile(dirOf(name) + "/" + kSceneFile, sceneJson);
}

bool ProjectStore::remove(std::string_view name) {
    if (!exists(name)) return false;
    if (slugFor(last()) == slugFor(name)) ::unlink((root_ + "/last").c_str());
    return removeTree(dirOf(name));
}

bool ProjectStore::rename(std::string_view from, std::string_view to) {
    if (!exists(from) || !validName(to)) return false;
    const std::string a = dirOf(from), b = dirOf(to);
    if (a != b) {
        if (isDir(b)) return false;   // another project already has that folder
        if (std::rename(a.c_str(), b.c_str()) != 0) return false;
    }
    if (slugFor(last()) == slugFor(from)) setLast(to);
    return writeInfo(b, to);
}

std::string ProjectStore::last() const {
    std::string s;
    if (!readFile(root_ + "/last", s)) return {};
    return trimName(s);
}

void ProjectStore::setLast(std::string_view name) {
    writeFileAtomic(root_ + "/last", trimName(name) + "\n");
}

std::string ProjectStore::uniqueName(std::string_view base) const {
    std::string b = trimName(base);
    if (!validName(b)) b = "Untitled";
    if (!exists(b)) return b;
    for (int i = 2; i < 10000; ++i) {
        std::string candidate = b.substr(0, kMaxName - 6) + " " + std::to_string(i);
        if (!exists(candidate)) return candidate;
    }
    return b;
}

}  // namespace mge::editor
