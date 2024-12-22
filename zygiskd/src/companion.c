#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>

#include <unistd.h>
#include <linux/limits.h>
#include <pthread.h>

#include <android/log.h>

#include "dl.h"
#include "utils.h"

typedef void (*zygisk_companion_entry)(int);

struct companion_module_thread_args {
  int fd;
  zygisk_companion_entry entry;
};

zygisk_companion_entry load_module(int fd) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

  void *handle = android_dlopen(path, RTLD_NOW);
  void *entry = dlsym(handle, "zygisk_companion_entry");

  return (zygisk_companion_entry)entry;
}

void *entry_thread(void *arg) {
  struct companion_module_thread_args *args = (struct companion_module_thread_args *)arg;

  int fd = args->fd;
  zygisk_companion_entry module_entry = args->entry;

  struct stat st0 = { 0 };
  if (fstat(fd, &st0) == -1) {
    LOGE("Failed to get initial client fd stats: %s\n", strerror(errno));

    free(args);

    return NULL;
  }

  module_entry(fd);

  /* INFO: Only attempt to close the client fd if it appears to be the same file
             and if we can successfully stat it again. This prevents double closes
             if the module companion already closed the fd.
  */
  struct stat st1;
  if (fstat(fd, &st1) == 0) {
    if (st0.st_dev == st1.st_dev && st0.st_ino == st1.st_ino) {
      LOGI("Client fd stats unchanged. Closing.\n");

      close(fd);
    } else {
      LOGI("Client fd stats changed, assuming module handled closing.\n");
    }
  }

  free(args);

  return NULL;
}

/* WARNING: Dynamic memory based */
void companion_entry(int fd) {
  LOGI("New companion entry.\n - Client fd: %d\n", fd);

  char name[256 + 1];
  ssize_t ret = read_string(fd, name, sizeof(name));
  if (ret == -1) {
    LOGE("Failed to read module name\n");

    /* TODO: Is that appropriate? */
    close(fd);

    exit(0);
  }

  LOGI(" - Module name: \"%s\"\n", name);

  int library_fd = read_fd(fd);
  if (library_fd == -1) {
    LOGE("Failed to receive library fd\n");

    /* TODO: Is that appropriate? */
    close(fd);

    exit(0);
  }

  LOGI(" - Library fd: %d\n", library_fd);

  zygisk_companion_entry module_entry = load_module(library_fd);
  close(library_fd);

  if (module_entry == NULL) {
    LOGE(" - No companion module entry for module: %s\n", name);

    ret = write_uint8_t(fd, 0);
    ASSURE_SIZE_WRITE("ZygiskdCompanion", "module_entry", ret, sizeof(uint8_t));

    exit(0);
  } else {
    LOGI(" - Module entry found\n");

    ret = write_uint8_t(fd, 1);
    ASSURE_SIZE_WRITE("ZygiskdCompanion", "module_entry", ret, sizeof(uint8_t));
  }

  while (1) {
    if (!check_unix_socket(fd, true)) {
      LOGE("Something went wrong in companion. Bye!\n");

      exit(0);
    }

    int client_fd = read_fd(fd);
    if (client_fd == -1) {
      LOGE("Failed to receive client fd\n");

      exit(0);
    }

    struct companion_module_thread_args *args = malloc(sizeof(struct companion_module_thread_args));
    if (args == NULL) {
      LOGE("Failed to allocate memory for thread args\n");

      close(client_fd);

      exit(0);
    }

    args->fd = client_fd;
    args->entry = module_entry;

    LOGI("New companion request.\n - Module name: %s\n - Client fd: %d\n", name, client_fd);

    ret = write_uint8_t(client_fd, 1);
    ASSURE_SIZE_WRITE("ZygiskdCompanion", "client_fd", ret, sizeof(uint8_t));

    pthread_t thread;
    pthread_create(&thread, NULL, entry_thread, args);
    pthread_detach(thread);

    LOGI(" - Spawned companion thread for client fd: %d\n", client_fd);
  }
}
