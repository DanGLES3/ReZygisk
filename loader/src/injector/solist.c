#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <dlfcn.h>

#include <linux/limits.h>

#include "elf_util.h"
#include "logging.h"

#include "solist.h"

/* TODO: Is offset for realpath necessary? It seems to have the function
           available anywhere. */
#ifdef __LP64__
  size_t solist_size_offset = 0x18;
  size_t solist_realpath_offset = 0x1a8;
#else
  size_t solist_size_offset = 0x90;
  size_t solist_realpath_offset = 0x174;
#endif

static const char *(*get_realpath_sym)(SoInfo *) = NULL;
static void (*soinfo_free)(SoInfo *) = NULL;
static SoInfo *(*find_containing_library)(const void *p) = NULL;
static void (*purge_unused_memory)(void) = NULL;

static inline const char *get_path(SoInfo *self) {
  if (get_realpath_sym)
    return (*get_realpath_sym)(self);

  return ((const char *)((uintptr_t)self + solist_realpath_offset));
}

static inline void set_size(SoInfo *self, size_t size) {
  *(size_t *) ((uintptr_t)self + solist_size_offset) = size;
}

struct pdg ppdg = { 0 };

static bool pdg_setup(ElfImg *img) {
  ppdg.ctor = (void *(*)())getSymbAddress(img,  "__dl__ZN18ProtectedDataGuardC2Ev");
  ppdg.dtor = (void *(*)())getSymbAddress(img, "__dl__ZN18ProtectedDataGuardD2Ev");

  return ppdg.ctor != NULL && ppdg.dtor != NULL;
}

/* INFO: Allow data to be written to the areas. */
static void pdg_unprotect() {
  (*ppdg.ctor)();
}

/* INFO: Block write and only allow read access to the areas. */
static void pdg_protect() {
  (*ppdg.dtor)();
}

static SoInfo *somain = NULL;

static size_t *g_module_load_counter = NULL;
static size_t *g_module_unload_counter = NULL;

static bool solist_init() {
  #ifdef __LP64__
    ElfImg *linker = ElfImg_create("/system/bin/linker64", NULL);
  #else
    ElfImg *linker = ElfImg_create("/system/bin/linker", NULL);
  #endif
  if (linker == NULL) {
    LOGE("Failed to load linker");

    return false;
  }

  if (!pdg_setup(linker)) {
    LOGE("Failed to setup pdg");

    ElfImg_destroy(linker);

    return false;
  }

  /* INFO: Since Android 15, the symbol names for the linker have a suffix,
              this makes it impossible to hardcode the symbol names. To allow
              this to work on all versions, we need to iterate over the loaded
              symbols and find the correct ones.

      See #63 for more information.
  */
  somain = (SoInfo *)getSymbValueByPrefix(linker, "__dl__ZL6somain");
  if (somain == NULL) {
    LOGE("Failed to find somain __dl__ZL6somain*");

    ElfImg_destroy(linker);

    return false;
  }

  LOGD("%p is somain", (void *)somain);

  get_realpath_sym = (const char *(*)(SoInfo *))getSymbAddress(linker, "__dl__ZNK6soinfo12get_realpathEv");
  if (get_realpath_sym == NULL) {
    LOGE("Failed to find get_realpath __dl__ZNK6soinfo12get_realpathEv");

    ElfImg_destroy(linker);

    somain = NULL;

    return false;
  }

  LOGD("%p is get_realpath", (void *)get_realpath_sym);

  soinfo_free = (void (*)(SoInfo *))getSymbAddressByPrefix(linker, "__dl__ZL11soinfo_freeP6soinfo");
  if (soinfo_free == NULL) {
    LOGE("Failed to find soinfo_free __dl__ZL11soinfo_freeP6soinfo*");

    ElfImg_destroy(linker);

    somain = NULL;

    return false;
  }

  LOGD("%p is soinfo_free", (void *)soinfo_free);

  find_containing_library = (SoInfo *(*)(const void *))getSymbAddress(linker, "__dl__Z23find_containing_libraryPKv");
  if (find_containing_library == NULL) {
    LOGE("Failed to find find_containing_library __dl__Z23find_containing_libraryPKv");

    ElfImg_destroy(linker);

    somain = NULL;

    return false;
  }

  LOGD("%p is find_containing_library", (void *)find_containing_library);

  purge_unused_memory = (void (*)())getSymbAddress(linker, "__dl__Z19purge_unused_memoryv");
  if (purge_unused_memory == NULL) {
    LOGE("Failed to find purge_unused_memory __dl__Z19purge_unused_memoryv");

    ElfImg_destroy(linker);

    somain = NULL;

    return false;
  }

  LOGD("%p is purge_unused_memory", (void *)purge_unused_memory);

  g_module_load_counter = (size_t *)getSymbAddress(linker, "__dl__ZL21g_module_load_counter");
  if (g_module_load_counter != NULL) LOGD("found symbol g_module_load_counter");

  g_module_unload_counter = (size_t *)getSymbAddress(linker, "__dl__ZL23g_module_unload_counter");
  if (g_module_unload_counter != NULL) LOGD("found symbol g_module_unload_counter");

  for (size_t i = 0; i < 1024 / sizeof(void *); i++) {
    size_t possible_size_of_somain = *(size_t *)((uintptr_t)somain + i * sizeof(void *));

    if (possible_size_of_somain < 0x100000 && possible_size_of_somain > 0x100) {
      solist_size_offset = i * sizeof(void *);

      LOGD("solist_size_offset is %zu * %zu = %p", i, sizeof(void *), (void *)solist_size_offset);

      break;
    }
  }

  ElfImg_destroy(linker);

  return true;
}

/* INFO: find_containing_library returns the SoInfo for the library that contains
           that memory inside its limits, hence why named "lib_memory" in ReZygisk. */
bool solist_drop_so_path(void *lib_memory, bool unload) {
  if (somain == NULL && !solist_init()) {
    LOGE("Failed to initialize solist");

    return false;
  }

  SoInfo *found = (*find_containing_library)(lib_memory);
  if (found == NULL) {
    LOGD("Could not find containing library for %p", lib_memory);

    purge_unused_memory();

    return false;
  }

  LOGD("Found so path for %p: %s", lib_memory, get_path(found));

  char path[PATH_MAX];
  if (get_path(found) == NULL) {
    LOGE("Failed to get path for %p", found);

    return false;
  }
  strncpy(path, get_path(found), sizeof(path) - 1);

  /* INFO: This area is guarded. Must unprotect first. */
  pdg_unprotect();
  set_size(found, 0);
  if (unload) pdg_protect();

  LOGD("Set size of %p to 0", (void *)found);

  /* INFO: We know that as libzygisk.so our limits, but modules are arbitrary, so
             calling deconstructors might break them. To avoid that, we manually call
             the separated structures, that however won't clean all traces in soinfo,
             not for now, at least. */
  if (unload && dlclose((void *)found) == -1) {
    LOGE("Failed to dlclose so path for %s: %s", path, dlerror());

    return false;
  } else if (!unload) {
    LOGD("Not unloading so path for %s, only dropping it", path);

    /* TODO: call notify_gdb_of_unload(found); (it is static) to avoid leaving traces in
               r_debug_tail.
       SOURCES:
        - https://android.googlesource.com/platform/bionic/+/refs/heads/main/linker/linker_gdb_support.cpp#94
    */
    /* INFO: unregister_soinfo_tls cannot be used since module might use JNI which may
               require TLS, so we cannot remove it. */
    soinfo_free(found);

    pdg_protect();
  }

  LOGD("Successfully hidden soinfo traces for %s", path);

  /* INFO: Avoid leaks by ensuring the freed places are munmapped */
  purge_unused_memory();

  LOGD("Purged unused memory successfully");

  /* INFO: Let's avoid trouble regarding detections */
  memset(path, strlen(path), 0);

  return true;
}

void solist_reset_counters(size_t load, size_t unload) {
  if (somain == NULL && !solist_init()) {
    LOGE("Failed to initialize solist");

    return;
  }

  if (g_module_load_counter == NULL || g_module_unload_counter == NULL) {
    LOGD("g_module counters not defined, skip reseting them");

    return;
  }

  size_t loaded_modules = *g_module_load_counter;
  size_t unloaded_modules = *g_module_unload_counter;

  if (loaded_modules >= load) {
    *g_module_load_counter -= load;

    LOGD("reset g_module_load_counter to %zu", *g_module_load_counter);
  }

  if (unloaded_modules >= unload) {
    *g_module_unload_counter -= unload;

    LOGD("reset g_module_unload_counter to %zu", *g_module_unload_counter);
  }
}
