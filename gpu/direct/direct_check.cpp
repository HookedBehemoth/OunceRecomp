// The validation messenger and strict mode (direct_check.h).
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "direct_check.h"
#include "direct_internal.h"

namespace hsd {

// ---- the known list (one place: tests/test_direct_strict.py reads it) ----
// Each entry: an event reason (as strict_event names it) or "validation:<message id>", and why strict reports it without
// stopping. An entry stays only while its cause is understood and cannot be fixed in the stage that added it.
const StrictKnown kStrictKnown[] = {
    {"Vulkan object made in steady play: an image for a texture the game made (renderable)",
     "the game makes render targets in play: a light's shadow map changes its size class, the dynamic resolution takes a level "
     "not seen before, a slot is made at a new offset (host/rt_slots.c). A transient target's image is an object bound in the "
     "transient pool at its storage offset: no device memory is allocated for it, and the object is kept for the next texture "
     "of that storage and shape (docs/direct/transient-pool.md)"},
    {"Vulkan object made in steady play: an attachment view of a game's texture",
     "the attachment view of such an image, made at its first SetRenderTargets (as the entry above)"},
    {"Vulkan object made in steady play: an image of a texture the game loaded (its prepared content)",
     "the game streams textures in play (walking: Hyrule Field's areas, NPCs, effects); each new content is one image, made at "
     "the texture's initialization and filled from the store, and destroyed when the game finalizes its last texture "
     ": a load the game does, not a per-frame object"},
    {nullptr, nullptr},
};
const size_t kStrictKnownCount = sizeof kStrictKnown / sizeof kStrictKnown[0] - 1;

namespace {

const StrictKnown* known(const char* reason) {
    for (size_t i = 0; i < kStrictKnownCount; i++)
        if (!std::strcmp(kStrictKnown[i].reason, reason)) return &kStrictKnown[i];
    return nullptr;
}

[[noreturn]] void stop(int code) {
    if (host && host->log_flush) host->log_flush();
    std::fflush(nullptr);
    _exit(code);
}

// ---- validation ----
std::mutex vmu;
struct Msg {
    int64_t n = 0;
    int severity = 0;
};
std::map<std::string, Msg> by_id;   // (under vmu)
std::atomic<int64_t> by_severity[4];   // (verbose, info, warning, error)
std::atomic<bool> status_heard{false};
bool layer_mapped = false, layer_available = false, layer_asked = false, messenger_made = false;
VkResult messenger_result = VK_NOT_READY;
VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
VkDebugUtilsMessengerCreateInfoEXT instance_messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
VkLayerSettingsCreateInfoEXT layer_settings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
VkLayerSettingEXT settings[1];
const VkBool32 kTrue = VK_TRUE;
const char* const kLayer = "VK_LAYER_KHRONOS_validation";
std::string layer_path;

int severity_index(VkDebugUtilsMessageSeverityFlagBitsEXT s) {
    return s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT     ? 3
           : s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? 2
           : s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT    ? 1
                                                                 : 0;
}
const char* const kSeverity[4] = {"verbose", "info", "warning", "error"};

VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                          const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* id = data && data->pMessageIdName ? data->pMessageIdName : "(no id)";
    const char* text = data && data->pMessage ? data->pMessage : "";
    const int s = severity_index(severity);
    // (the layer's own line at instance creation: proof that it runs, not a finding)
    if (std::strstr(id, "status-message") || !std::strcmp(id, "CURRENT-VALIDATION-ENABLED") || std::strstr(text, "Khronos Validation Layer Active")) {
        if (!status_heard.exchange(true)) {
            char line[1900];
            std::snprintf(line, sizeof line, "validation: the layer's status message heard (%s, %s): %s", kSeverity[s], id, text);
            for (char* c = line; *c; c++)
                if (*c == '\n') *c = ' ';
            log_long(line, "validation");
        }
        return VK_FALSE;
    }
    // (the loader's line that the layer was forced on by its environment: how it got there, not a finding)
    if (!std::strcmp(id, "Loader Message") && std::strstr(text, "VK_LAYER_KHRONOS_validation") && std::strstr(text, "forced enabled")) {
        logf("validation: the loader: %s", text);
        return VK_FALSE;
    }
    by_severity[s].fetch_add(1, std::memory_order_relaxed);
    bool first;
    {
        std::lock_guard lk(vmu);
        Msg& m = by_id[std::string(kSeverity[s]) + " " + id];   // (by severity and id: the loader's lines share one id)
        first = m.n++ == 0;
        m.severity = s;
    }
    if (first) {
        char line[1900];
        std::snprintf(line, sizeof line, "validation: first %s %s (types %#x, objects %u): %s", kSeverity[s], id, (unsigned)types,
                      data ? data->objectCount : 0u, text);
        log_long(line, "validation");
    }
    if (strict_on() && s >= 2) {
        const std::string key = std::string("validation:") + id;
        if (const StrictKnown* k = known(key.c_str())) {
            if (first) logf("strict: known %s (%s): reported, the run goes on", key.c_str(), k->why);
        } else {
            logf("strict: STOP at present %llu: validation %s %s", (unsigned long long)present_count(), kSeverity[s], id);
            stop(87);
        }
    }
    return VK_FALSE;
}

int find_layer(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && std::strstr(info->dlpi_name, "VkLayer_khronos_validation")) {
        layer_path = info->dlpi_name;
        return 1;
    }
    return 0;
}

void summary_at_exit() {
    char b[2048];
    validation_stats(b, sizeof b);
    logf("validation: at exit: %s", b);
    strict_stats(b, sizeof b);
    logf("strict: at exit: %s", b);
}

// ---- strict ----
std::mutex smu;
struct Ev {
    int64_t before = 0, after = 0;
};
std::map<std::string, Ev> events;   // (under smu)
std::atomic<bool> armed{false};
std::atomic<uint64_t> armed_at{0}, last_miss{0};
std::atomic<int64_t> misses_before{0};
thread_local int debug_objects = 0;
thread_local const char* objects_for = nullptr;   // (ObjectsFor)

uint64_t strict_from() {
    static const uint64_t f = [] {
        const char* v = dbg_value("strict");
        return v && atoll(v) > 0 ? (uint64_t)atoll(v) : 1200ull;
    }();
    return f;
}

}  // namespace

bool strict_enabled = dbg("strict") || dbg_value("strict") != nullptr;

void validation_instance_info(VkInstanceCreateInfo& ci, std::vector<const char*>& exts, const std::vector<VkExtensionProperties>& all) {
    auto enum_layers = (PFN_vkEnumerateInstanceLayerProperties)get_instance_proc(nullptr, "vkEnumerateInstanceLayerProperties");
    uint32_t n = 0;
    if (enum_layers) enum_layers(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    if (n) enum_layers(&n, layers.data());
    for (const auto& l : layers) layer_available |= !std::strcmp(l.layerName, kLayer);
    bool has_utils = false;
    for (const auto& e : all) has_utils |= !std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (dbg("validation") && layer_available) {   // (HOS_DBG=validation: the layer and its synchronization validation, asked here)
        static const char* names[1] = {kLayer};
        ci.enabledLayerCount = 1, ci.ppEnabledLayerNames = names;
        settings[0] = {kLayer, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &kTrue};
        layer_settings.settingCount = 1, layer_settings.pSettings = settings;
        layer_settings.pNext = ci.pNext, ci.pNext = &layer_settings;
        layer_asked = true;
        has_utils = true;   // (the layer has it)
    }
    if (!has_utils) return;
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    instance_messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    instance_messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    instance_messenger.pfnUserCallback = on_message;
    instance_messenger.pNext = ci.pNext, ci.pNext = &instance_messenger;
}

void validation_instance_made(VkInstance instance) {
    dl_iterate_phdr(find_layer, nullptr);
    layer_mapped = !layer_path.empty();
    if (layer_mapped && instance_messenger.pfnUserCallback) {
        auto make = (PFN_vkCreateDebugUtilsMessengerEXT)get_instance_proc(instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT ci = instance_messenger;
        ci.pNext = nullptr;
        messenger_result = make ? make(instance, &ci, nullptr, &messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
        messenger_made = messenger_result == VK_SUCCESS;
    }
    if (layer_mapped)
        logf("validation: the layer is loaded: %s mapped in the process (%s; installed: %s); messenger %s (VkResult %d), status "
             "message %s; warnings and errors counted%s",
             layer_path.c_str(), layer_asked ? "asked by HOS_DBG=validation" : "enabled outside the back end (VK_LOADER_LAYERS_ENABLE)",
             layer_available ? "yes" : "no", messenger_made ? "made" : "NOT made", (int)messenger_result,
             status_heard.load() ? "heard" : "not heard", strict_on() ? ", strict: any warning or error ends the run" : "");
    else
        logf("validation: off (no validation layer in the process; layer installed: %s)", layer_available ? "yes" : "no");
    if (strict_on()) {
        logf("strict: on: armed at the first present >= %llu that is %llu presents after the last program miss; known list %zu entries%s",
             (unsigned long long)strict_from(), (unsigned long long)kStrictQuiet, kStrictKnownCount, kStrictKnownCount ? ":" : "");
        for (size_t i = 0; i < kStrictKnownCount; i++) logf("strict: known: %s: %s", kStrictKnown[i].reason, kStrictKnown[i].why);
    }
    if (layer_mapped || strict_on()) std::atexit(summary_at_exit);
}

void validation_stats(char* out, size_t cap) {
    if (!layer_mapped) {
        std::snprintf(out, cap, "off (no layer in the process)");
        return;
    }
    int k = std::snprintf(out, cap, "layer %s, messenger %s, status message %s; messages: %lld errors, %lld warnings, %lld info; distinct ids",
                          layer_path.c_str(), messenger_made ? "made" : "NOT made", status_heard.load() ? "heard" : "not heard",
                          (long long)by_severity[3].load(), (long long)by_severity[2].load(), (long long)by_severity[1].load());
    std::lock_guard lk(vmu);
    k += std::snprintf(out + k, cap - k, " %zu%s", by_id.size(), by_id.empty() ? "" : ":");
    for (const auto& [id, m] : by_id) {
        if (k > (int)cap - 120) {
            std::snprintf(out + k, cap - k, " ...");
            break;
        }
        k += std::snprintf(out + k, cap - k, " %s x%lld;", id.c_str(), (long long)m.n);
    }
}

void strict_event(const char* reason, const char* where, ...) {
    if (!strict_enabled) return;
    char w[1500];
    va_list a;
    va_start(a, where);
    std::vsnprintf(w, sizeof w, where, a);
    va_end(a);
    const bool on = armed.load(std::memory_order_acquire);
    const StrictKnown* k = known(reason);
    int64_t n;
    {
        std::lock_guard lk(smu);
        Ev& e = events[reason];
        n = on ? e.after++ : e.before++;
    }
    const unsigned long long p = (unsigned long long)present_count();
    if (!on) {
        if (n < 2) logf("strict: before arming, present %llu: %s: %s", p, reason, w);
        return;
    }
    if (k) {
        if (n < 3) logf("strict: known event at present %llu: %s: %s (known: %s)", p, reason, w, k->why);
        return;
    }
    static const bool survey = dbg("strictall");   // (HOS_DBG=strict,strictall: a survey, every reason's first events logged, no stop)
    if (survey) {
        if (n < 3) logf("strict: event at present %llu (survey, not stopped): %s: %s", p, reason, w);
        return;
    }
    logf("strict: STOP at present %llu (armed at %llu): %s: %s", p, (unsigned long long)armed_at.load(), reason, w);
    char b[2048];
    strict_stats(b, sizeof b);
    logf("strict: %s", b);
    stop(86);
}

void strict_program_miss(const char* where) {
    if (!strict_enabled) return;
    if (armed.load(std::memory_order_acquire)) return strict_event("program miss in play", "%s", where);
    last_miss.store(present_count(), std::memory_order_relaxed);
    misses_before.fetch_add(1, std::memory_order_relaxed);
}

void strict_present(uint64_t present) {
    if (!strict_enabled || armed.load(std::memory_order_relaxed)) return;
    const uint64_t m = last_miss.load(std::memory_order_relaxed);
    if (present >= strict_from() && present >= m + kStrictQuiet) {
        armed_at.store(present);
        armed.store(true, std::memory_order_release);
        logf("strict: armed at present %llu (from %llu; the last program miss at present %llu, %lld misses before)", (unsigned long long)present,
             (unsigned long long)strict_from(), (unsigned long long)m, (long long)misses_before.load());
    }
}

bool strict_armed() { return armed.load(std::memory_order_acquire); }

void strict_stats(char* out, size_t cap) {
    if (!strict_enabled) {
        std::snprintf(out, cap, "off");
        return;
    }
    int k = std::snprintf(out, cap, "%s (at present %llu); known list %zu; events by reason (before arming / after):",
                          armed.load() ? "armed" : "not armed", (unsigned long long)armed_at.load(), kStrictKnownCount);
    std::lock_guard lk(smu);
    if (events.empty()) std::snprintf(out + k, cap - k, " none");
    for (const auto& [r, e] : events) {
        if (k > (int)cap - 160) {
            std::snprintf(out + k, cap - k, " ...");
            break;
        }
        k += std::snprintf(out + k, cap - k, " %s %lld / %lld%s;", r.c_str(), (long long)e.before, (long long)e.after, known(r.c_str()) ? " (known)" : "");
    }
}

void object_made(int n, const char* what) {
    st_objects.fetch_add(n, std::memory_order_relaxed);
    if (strict_enabled && !debug_objects && armed.load(std::memory_order_relaxed)) {
        if (objects_for) strict_event((std::string("Vulkan object made in steady play: ") + objects_for).c_str(), "%s (%d)", what, n);
        else strict_event("Vulkan object made in steady play", "%s (%d)", what, n);
    }
}
DebugObjects::DebugObjects() { debug_objects++; }
DebugObjects::~DebugObjects() { debug_objects--; }
ObjectsFor::ObjectsFor(const char* what) : prev(objects_for) { objects_for = what; }
ObjectsFor::~ObjectsFor() { objects_for = prev; }

}  // namespace hsd
