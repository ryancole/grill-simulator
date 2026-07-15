#include "actions.hpp"

#include "input.hpp"

#include <toml++/toml.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

// One action's place in the config: the field name it reads from the [bindings]
// table, and the key it falls back to when the file does not name it. The default
// is spelled as a key name (not a raw VK code) so it goes through the very same
// parser the file does -- one source of truth for what a key name means.
//
// `display` is the label the in-game keybinds screen shows, and `rebindable` marks
// the gameplay actions that screen may edit -- the developer shortcuts and the menu
// navigation are deliberately fixed, so they are not offered for rebinding.
struct ActionSpec {
    Action action;
    const char* toml_key;
    const char* default_key;
    const char* display;
    bool rebindable;
};

constexpr ActionSpec kSpecs[] = {
    {Action::MoveForward, "move_forward", "W", "Move Forward", true},
    {Action::MoveBack, "move_back", "S", "Move Back", true},
    {Action::MoveLeft, "move_left", "A", "Move Left", true},
    {Action::MoveRight, "move_right", "D", "Move Right", true},
    {Action::Jump, "jump", "Space", "Jump", true},
    {Action::Sprint, "sprint", "Shift", "Sprint", true},
    {Action::Interact, "interact", "E", "Interact", true},
    {Action::ReloadLevel, "reload_level", "R", "Reload Level", false},
    {Action::SelectLevel1, "select_level_1", "1", "Select Level 1", false},
    {Action::SelectLevel2, "select_level_2", "2", "Select Level 2", false},
    {Action::ToggleDebug, "toggle_debug", "Backtick", "Toggle Debug", false},
    {Action::MenuUp, "menu_up", "Up", "Menu Up", false},
    {Action::MenuDown, "menu_down", "Down", "Menu Down", false},
    {Action::MenuConfirm, "menu_confirm", "Enter", "Menu Confirm", false},
};

// Every action must carry a spec, or its binding would never be seeded. Guards
// against adding an Action above and forgetting to bind it here.
static_assert(std::size(kSpecs) == static_cast<std::size_t>(Action::Count),
              "every Action needs a spec in kSpecs");

std::size_t Index(Action action) { return static_cast<std::size_t>(action); }

// Resolves a human-readable key name to a Win32 virtual-key code, or nullopt when
// the name is not one this game knows. Case-insensitive. A single letter or digit
// maps to its own VK code (VK_A == 'A', VK_0 == '0'); everything else is a named
// key in the table below. This is the whole vocabulary controls.toml may use.
std::optional<int> KeyFromName(std::string name) {
    for (char& c : name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    if (name.size() == 1) {
        const char c = name[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            return static_cast<int>(c);
        }
    }

    static const std::unordered_map<std::string, int> named = {
        {"SPACE", VK_SPACE},       {"SHIFT", VK_SHIFT},
        {"CTRL", VK_CONTROL},      {"CONTROL", VK_CONTROL},
        {"ALT", VK_MENU},          {"ENTER", VK_RETURN},
        {"RETURN", VK_RETURN},     {"TAB", VK_TAB},
        {"ESC", VK_ESCAPE},        {"ESCAPE", VK_ESCAPE},
        {"BACKSPACE", VK_BACK},    {"UP", VK_UP},
        {"DOWN", VK_DOWN},         {"LEFT", VK_LEFT},
        {"RIGHT", VK_RIGHT},
        // The key left of 1 / above Tab. Its several common names all land on the same
        // VK, so a config can spell it whichever way reads best.
        {"`", VK_OEM_3},           {"BACKTICK", VK_OEM_3},
        {"GRAVE", VK_OEM_3},       {"TILDE", VK_OEM_3},
        {"F1", VK_F1},
        {"F2", VK_F2},             {"F3", VK_F3},
        {"F4", VK_F4},             {"F5", VK_F5},
        {"F6", VK_F6},             {"F7", VK_F7},
        {"F8", VK_F8},             {"F9", VK_F9},
        {"F10", VK_F10},           {"F11", VK_F11},
        {"F12", VK_F12},
    };
    const auto it = named.find(name);
    if (it != named.end()) {
        return it->second;
    }
    return std::nullopt;
}

// The inverse of KeyFromName: a virtual-key code back to a name the keybinds screen
// can show and SaveUserOverrides can write. It picks one canonical spelling per key
// (KeyFromName accepts several for a few), and every name it returns round-trips back
// through KeyFromName to the same VK, so a saved override always reloads. Returns
// nullopt for a code outside this game's vocabulary.
std::optional<std::string> NameFromKey(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }

    static const std::unordered_map<int, std::string> names = {
        {VK_SPACE, "Space"},   {VK_SHIFT, "Shift"},   {VK_CONTROL, "Ctrl"},
        {VK_MENU, "Alt"},      {VK_RETURN, "Enter"},  {VK_TAB, "Tab"},
        {VK_ESCAPE, "Esc"},    {VK_BACK, "Backspace"},{VK_UP, "Up"},
        {VK_DOWN, "Down"},     {VK_LEFT, "Left"},     {VK_RIGHT, "Right"},
        {VK_OEM_3, "Backtick"},
        {VK_F1, "F1"},         {VK_F2, "F2"},         {VK_F3, "F3"},
        {VK_F4, "F4"},         {VK_F5, "F5"},         {VK_F6, "F6"},
        {VK_F7, "F7"},         {VK_F8, "F8"},         {VK_F9, "F9"},
        {VK_F10, "F10"},       {VK_F11, "F11"},       {VK_F12, "F12"},
    };
    const auto it = names.find(vk);
    if (it != names.end()) {
        return it->second;
    }
    return std::nullopt;
}

// A malformed-config error naming the file, matching the level loader's style so a
// broken controls.toml reports itself the same way a broken level does.
[[noreturn]] void Fail(const std::filesystem::path& path, const std::string& what) {
    throw std::runtime_error(path.filename().string() + ": " + what);
}

// Reads one binding value: either a single key name ("W") or an array of them
// (["W", "Up"]), into the list of VK codes any of which drives the action. An
// unknown key name, or a value that is not a string/array-of-strings, fails naming
// the file and the field. An empty array is allowed -- it deliberately unbinds the
// action, which is a valid choice.
std::vector<int> ParseBinding(const toml::node& node, const std::filesystem::path& path,
                              const char* field) {
    std::vector<int> keys;
    const auto add = [&](const toml::node& value) {
        const std::optional<std::string> name = value.value<std::string>();
        if (!name) {
            Fail(path, std::string(field) + " must be a key name or a list of key names");
        }
        const std::optional<int> vk = KeyFromName(*name);
        if (!vk) {
            Fail(path, "unknown key \"" + *name + "\" bound to " + field);
        }
        keys.push_back(*vk);
    };

    if (const toml::array* array = node.as_array()) {
        for (const toml::node& value : *array) {
            add(value);
        }
    } else {
        add(node);
    }
    return keys;
}

} // namespace

Actions::Actions() { ResetToDefaults(); }

void Actions::ResetToDefaults() {
    for (const ActionSpec& spec : kSpecs) {
        // The defaults are compiled-in constants, so KeyFromName cannot legitimately
        // fail on them; guard anyway rather than dereference a nullopt.
        if (const std::optional<int> vk = KeyFromName(spec.default_key)) {
            bindings_[Index(spec.action)] = {*vk};
        }
    }
}

void Actions::LoadFromFile(const std::filesystem::path& path) {
    // An absent file keeps the defaults the constructor seeded: the game plays with
    // no controls.toml present, exactly as it did before the file existed.
    if (!std::filesystem::exists(path)) {
        return;
    }

    toml::table parsed;
    try {
        parsed = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        // toml++ carries the line/column of the syntax error; surface it with the
        // file name so a broken controls file names itself, as the levels do.
        std::ostringstream message;
        message << path.filename().string() << ":" << error.source().begin.line << ": "
                << error.description();
        throw std::runtime_error(message.str());
    }

    const toml::table* bindings = parsed["bindings"].as_table();
    if (bindings == nullptr) {
        return; // No [bindings] table: nothing overrides the defaults.
    }

    for (const ActionSpec& spec : kSpecs) {
        const toml::node_view<const toml::node> node = (*bindings)[spec.toml_key];
        if (!node) {
            continue; // Unnamed in the file: this action keeps its default.
        }
        bindings_[Index(spec.action)] = ParseBinding(*node.node(), path, spec.toml_key);
    }
}

std::vector<Actions::Binding> Actions::RebindableBindings() const {
    std::vector<Binding> result;
    for (const ActionSpec& spec : kSpecs) {
        if (!spec.rebindable) {
            continue;
        }
        const std::vector<int>& keys = bindings_[Index(spec.action)];
        // The screen edits the primary (first) key; an action bound to nothing reads
        // as "Unbound". A key with no canonical name cannot occur through the in-game
        // path (capture only accepts named keys), but a hand-edited controls.toml could
        // sneak one in, so fall back rather than show nothing.
        std::string key = "Unbound";
        if (!keys.empty()) {
            key = NameFromKey(keys.front()).value_or("?");
        }
        result.push_back({spec.action, spec.display, std::move(key)});
    }
    return result;
}

void Actions::Rebind(Action action, int key) {
    // Only the gameplay actions are rebindable; ignore a request for anything else.
    const auto* spec = std::find_if(std::begin(kSpecs), std::end(kSpecs),
                                    [&](const ActionSpec& s) { return s.action == action; });
    if (spec == std::end(kSpecs) || !spec->rebindable) {
        return;
    }

    // No two gameplay actions may answer to the same key: strip the incoming key from
    // every other rebindable action first, so the one being set is the sole owner and
    // whatever held it before is left unbound (the player can rebind that one next).
    for (const ActionSpec& other : kSpecs) {
        if (!other.rebindable || other.action == action) {
            continue;
        }
        std::vector<int>& keys = bindings_[Index(other.action)];
        keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
    }

    bindings_[Index(action)] = {key};
}

void Actions::SaveUserOverrides(const std::filesystem::path& path) const {
    // Mirror controls.toml's shape: a [bindings] table naming each rebindable action's
    // primary key. An unbound action, or one whose key has no writable name, is emitted
    // as an empty list -- which the loader reads as a deliberate unbind, round-tripping.
    toml::table bindings;
    for (const ActionSpec& spec : kSpecs) {
        if (!spec.rebindable) {
            continue;
        }
        const std::vector<int>& keys = bindings_[Index(spec.action)];
        std::optional<std::string> name;
        if (!keys.empty()) {
            name = NameFromKey(keys.front());
        }
        if (name) {
            bindings.insert(spec.toml_key, *name);
        } else {
            bindings.insert(spec.toml_key, toml::array{});
        }
    }

    toml::table root;
    root.insert("bindings", std::move(bindings));

    // A save that cannot open the file is swallowed: it means the next launch falls
    // back to the committed defaults, not a crash in the middle of the options menu.
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# Per-user control overrides, written by the in-game keybinds editor.\n"
        << "# Layered over assets/controls.toml at startup; rebinding in-game rewrites\n"
        << "# this file, so hand edits here are not preserved.\n\n";
    out << root << '\n';
}

void Actions::Update(const Input& input) {
    previous_ = current_;
    for (std::size_t i = 0; i < kCount; ++i) {
        bool active = false;
        for (const int vk : bindings_[i]) {
            if (input.IsKeyDown(vk)) {
                active = true;
                break;
            }
        }
        current_.set(i, active);
    }
}

bool Actions::IsActive(Action action) const {
    return current_.test(Index(action));
}

bool Actions::WasPressed(Action action) const {
    const std::size_t i = Index(action);
    return current_.test(i) && !previous_.test(i);
}

bool Actions::WasReleased(Action action) const {
    const std::size_t i = Index(action);
    return !current_.test(i) && previous_.test(i);
}
