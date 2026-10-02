#pragma once
#include "configuration.hpp"
namespace lfh {
std::wstring InstallDirectory();
std::wstring ConfigPath();
std::wstring ModuleDirectory();
std::wstring DeploymentStatus();
bool IsInstalled();
void EnsureConfig();
void Install(const Configuration& config);
void Apply(const Configuration& config);
void Uninstall();
}
