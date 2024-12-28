#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>

#include <android/log.h>
#include <android/dlext.h>

#include "companion.h"
#include "utils.h"

#define __LOADER_ANDROID_CREATE_NAMESPACE_TYPE(name) struct android_namespace_t *(*name)( \
  const char *name,                                                                      \
  const char *ld_library_path,                                                           \
  const char *default_library_path,                                                      \
  uint64_t type,                                                                         \
  const char *permitted_when_isolated_path,                                              \
  struct android_namespace_t *parent,                                                    \
  const void *caller_addr)

void *dlopen_ext(const char* path, int flags) {
  android_dlextinfo info = { 0 };
  char *dir = dirname(path);

  __LOADER_ANDROID_CREATE_NAMESPACE_TYPE(__loader_android_create_namespace) = (__LOADER_ANDROID_CREATE_NAMESPACE_TYPE( ))dlsym(RTLD_DEFAULT, "__loader_android_create_namespace");

  struct android_namespace_t *ns = __loader_android_create_namespace == NULL ? NULL :
              __loader_android_create_namespace(path, dir, NULL,
                                                2, /* ANDROID_NAMESPACE_TYPE_SHARED */
                                                NULL, NULL,
                                                (void *)&dlopen_ext);

  if (ns) {
    info.flags = ANDROID_DLEXT_USE_NAMESPACE;
    info.library_namespace = ns;

    LOGI("Open %s with namespace %p", path, (void *)ns);
  } else {
    LOGW("Cannot create namespace for %s", path);
  }

  void *handle = android_dlopen_ext(path, flags, &info);
  if (handle) {
    LOGI("dlopen %s: %p", path, handle);
  } else {
    LOGE("dlopen %s: %s", path, dlerror());
  }

  return handle;
}
