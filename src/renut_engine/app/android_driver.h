#pragma once
// Android: custom Vulkan drivers (Turnip and friends, in the AdrenoTools
// zip format: meta.json + the driver .so) loaded through libadrenotools, plus
// the few calls into the Java activity the manager needs.
//
// Installed drivers live in the app's internal files folder (dlopen refuses
// shared storage): <files>/drivers/<id>/. The selection is in
// <files>/driver.txt; empty or missing means the system driver.

#include <filesystem>
#include <string>
#include <vector>

namespace renut::android {

struct Driver {
    std::string id;           // folder name under drivers/
    std::string name;         // meta.json name (+ version)
    std::string description;
    std::string library;      // meta.json libraryName
};

std::filesystem::path DriversFolder();
std::vector<Driver> InstalledDrivers();
// AdrenoTools driver zips waiting in /sdcard/reNut/drivers.
std::vector<std::filesystem::path> DriverZips();
// Unpacks a driver zip into DriversFolder(); the new id, or empty with *error.
std::string InstallDriverZip(const std::filesystem::path &zip, std::string *error);
void RemoveDriver(const std::string &id);

std::string SelectedDriver();
void SelectDriver(const std::string &id);

// Before the Vulkan interface is created: loads the selected driver and points
// volk at it. A driver that took the last run down (no MarkDriverWorking) is
// dropped for the system driver.
void LoadVulkanDriver();
// Once the device is up with the loaded driver.
void MarkDriverWorking();
// What LoadVulkanDriver did, for the manager.
const std::string &DriverStatus();
// The id LoadVulkanDriver loaded ("" = system driver).
const std::string &LoadedDriver();

// Java side (RenutActivity).
bool HasStorageAccess();
void RequestStorageAccess();
// Starts a new process: a driver is loaded once per process.
void RestartApp();

} // namespace renut::android
