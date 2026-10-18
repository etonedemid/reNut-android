#include "renut_engine/app/android_driver.h"

#include <dlfcn.h>
#include <jni.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include <SDL3/SDL_system.h>
#include <adrenotools/driver.h>
#include <adrenotools/priv.h>
#include <plume_vulkan.h>
#include <rex/logging.h>

#include "renut_engine/mods/json.h"
#include "renut_engine/mods/zip_reader.h"

namespace fs = std::filesystem;

namespace renut::android {
namespace {

std::string g_status = "System driver";
std::string g_loaded;

fs::path FilesFolder() {
    const char *internal = SDL_GetAndroidInternalStoragePath();
    return internal ? fs::path(internal) : fs::path("/data/local/tmp");
}

fs::path SelectionFile() { return FilesFolder() / "driver.txt"; }
// Written before a custom driver loads, removed once it works.
fs::path PendingFile() { return FilesFolder() / "driver_pending.txt"; }

std::string ReadLine(const fs::path &path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

void WriteLine(const fs::path &path, const std::string &line) {
    std::ofstream(path, std::ios::trunc) << line << "\n";
}

std::optional<Driver> ReadDriver(const fs::path &folder) {
    std::ifstream in(folder / "meta.json");
    if (!in)
        return std::nullopt;
    std::stringstream text;
    text << in.rdbuf();
    auto meta = renut::mods::ParseJson(text.str(), nullptr);
    if (!meta)
        return std::nullopt;
    Driver d;
    d.id = folder.filename().string();
    d.name = meta->String("name", d.id);
    const std::string version = meta->String("driverVersion");
    if (!version.empty())
        d.name += " " + version;
    d.description = meta->String("description");
    d.library = meta->String("libraryName");
    if (d.library.empty() || !fs::exists(folder / d.library))
        return std::nullopt;
    return d;
}

// The folder librenut.so was extracted to (ApplicationInfo.nativeLibraryDir),
// where adrenotools' hook libraries are too.
std::string NativeLibraryDir() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&NativeLibraryDir), &info) || !info.dli_fname)
        return {};
    return fs::path(info.dli_fname).parent_path().string() + "/";
}

// Calls a method of RenutActivity: "()V" or "()Z".
bool CallActivity(const char *method, const char *signature) {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return false;
    bool result = false;
    jclass cls = env->GetObjectClass(activity);
    if (jmethodID id = env->GetMethodID(cls, method, signature)) {
        if (signature[2] == 'Z')
            result = env->CallBooleanMethod(activity, id);
        else
            env->CallVoidMethod(activity, id);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return result;
}

} // namespace

fs::path DriversFolder() { return FilesFolder() / "drivers"; }

std::vector<Driver> InstalledDrivers() {
    std::vector<Driver> drivers;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(DriversFolder(), ec)) {
        if (entry.is_directory())
            if (auto d = ReadDriver(entry.path()))
                drivers.push_back(std::move(*d));
    }
    std::sort(drivers.begin(), drivers.end(),
              [](const Driver &a, const Driver &b) { return a.name < b.name; });
    return drivers;
}

std::vector<fs::path> DriverZips() {
    std::vector<fs::path> zips;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sdcard/reNut/drivers", ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".zip")
            zips.push_back(entry.path());
    }
    std::sort(zips.begin(), zips.end());
    return zips;
}

std::string InstallDriverZip(const fs::path &zip, std::string *error) {
    renut::mods::ZipReader reader;
    if (!reader.Open(zip, error))
        return {};
    auto meta_bytes = reader.Read("meta.json", error);
    if (!meta_bytes) {
        *error = "no meta.json: not an AdrenoTools driver zip";
        return {};
    }
    const std::string meta_text(meta_bytes->begin(), meta_bytes->end());
    auto meta = renut::mods::ParseJson(meta_text, error);
    if (!meta)
        return {};
    const std::string library = meta->String("libraryName");
    if (library.empty() || library.find('/') != std::string::npos) {
        *error = "meta.json has no usable libraryName";
        return {};
    }
    auto library_bytes = reader.Read(library, error);
    if (!library_bytes)
        return {};

    const std::string id = zip.stem().string();
    const fs::path folder = DriversFolder() / id;
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder, ec);
    std::ofstream(folder / "meta.json", std::ios::binary).write(meta_text.data(), meta_text.size());
    std::ofstream(folder / library, std::ios::binary)
        .write(reinterpret_cast<const char *>(library_bytes->data()), library_bytes->size());
    if (!ReadDriver(folder)) {
        *error = "could not write the driver files";
        fs::remove_all(folder, ec);
        return {};
    }
    return id;
}

void RemoveDriver(const std::string &id) {
    if (id.empty() || id.find('/') != std::string::npos)
        return;
    std::error_code ec;
    fs::remove_all(DriversFolder() / id, ec);
    if (SelectedDriver() == id)
        SelectDriver({});
}

std::string SelectedDriver() { return ReadLine(SelectionFile()); }

void SelectDriver(const std::string &id) { WriteLine(SelectionFile(), id); }

void LoadVulkanDriver() {
    std::string id = SelectedDriver();
    std::error_code ec;
    if (fs::exists(PendingFile(), ec)) {
        const std::string crashed = ReadLine(PendingFile());
        fs::remove(PendingFile(), ec);
        if (!crashed.empty() && crashed == id) {
            SelectDriver({});
            g_status = crashed + " stopped the last run: back to the system driver";
            REXLOG_WARN("Vulkan driver {} did not come up last run; using the system driver", crashed);
            return;
        }
    }
    if (id.empty())
        return;
    auto driver = ReadDriver(DriversFolder() / id);
    if (!driver) {
        SelectDriver({});
        g_status = id + " is missing: system driver";
        return;
    }

    WriteLine(PendingFile(), id);
    const std::string hooks = NativeLibraryDir();
    const std::string folder = (DriversFolder() / id).string() + "/";
    void *handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
                                              nullptr, hooks.c_str(), folder.c_str(),
                                              driver->library.c_str(), nullptr, nullptr);
    auto gipa = handle ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                             dlsym(handle, "vkGetInstanceProcAddr"))
                       : nullptr;
    if (!gipa) {
        fs::remove(PendingFile(), ec);
        g_status = driver->name + " failed to load: system driver";
        const char *why = dlerror();
        REXLOG_ERROR("adrenotools could not load {} ({}): {}", driver->name, folder,
                     why ? why : "no vkGetInstanceProcAddr");
        return;
    }
    volkInitializeCustom(gipa);
    g_loaded = id;
    g_status = driver->name;
    REXLOG_INFO("Vulkan driver: {} ({})", driver->name, folder);
}

void MarkDriverWorking() {
    std::error_code ec;
    fs::remove(PendingFile(), ec);
}

const std::string &DriverStatus() { return g_status; }
const std::string &LoadedDriver() { return g_loaded; }

bool HasStorageAccess() { return CallActivity("hasStorageAccess", "()Z"); }
void RequestStorageAccess() { CallActivity("requestStorageAccess", "()V"); }
void RestartApp() { CallActivity("restartApp", "()V"); }

} // namespace renut::android
