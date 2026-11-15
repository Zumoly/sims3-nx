/**
 * @file        rex/core/dynlib_switch.cpp
 * @brief       DynamicLibrary backend for Horizon, with a static module registry
 *
 * Sims3-NX overlay of katamari-nx's file. An NRO has no dlopen, so "loading" a library
 * means finding a set of symbols that were linked into the NRO and registered at start
 * up. Names that were not registered still report "not loaded", as before, so the SDK's
 * optional loaders (RenderDoc, SPIRV-Tools) keep their behaviour.
 *
 * Sims 3 keeps its game code in Sims3.xex, a guest DLL. Its recompiled code is linked
 * statically and registered as "sims3_Sims3", the name codegen gives that module.
 */

#include <map>
#include <string>

#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Nintendo Switch-only");

namespace rex::platform {

namespace {
using SymbolTable = std::map<std::string, void*>;
std::map<std::string, SymbolTable>& Libraries() {
  static std::map<std::string, SymbolTable> libraries;
  return libraries;
}
}  // namespace

// Declared by the title: this is not part of the SDK's public header.
void RegisterStaticLibrarySymbol(const char* library, const char* symbol, void* address) {
  Libraries()[library][symbol] = address;
}

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution mode) {
  (void)mode;
  Close();
  auto it = Libraries().find(path.string());
  if (it == Libraries().end()) {
    return false;
  }
  handle_ = &it->second;
  return true;
}

void DynamicLibrary::Close() {
  handle_ = nullptr;
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (!handle_) {
    return nullptr;
  }
  const auto* table = static_cast<const SymbolTable*>(handle_);
  auto it = table->find(name);
  return it == table->end() ? nullptr : it->second;
}

}  // namespace rex::platform
