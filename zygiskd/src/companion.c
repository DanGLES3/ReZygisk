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

#include "companion.h"
#include "dl.h"
#include "utils.h"

typedef void (*zygisk_companion_entry_func)(int);

struct companion_module_thread_args {
  int fd;
  zygisk_companion_entry_func entry;
};

zygisk_companion_entry_func load_module(int fd) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

  void *handle = android_dlopen(path, RTLD_NOW);
  void *entry = dlsym(handle, "zygisk_companion_entry");
  
  return (zygisk_companion_entry_func)entry;
}

void *entry_thread(void *arg) {
  struct companion_module_thread_args *args = (struct companion_module_thread_args *)arg;

  int fd = args->fd;
  zygisk_companion_entry_func module_entry = args->entry;

  struct stat st0;
  if (fstat(fd, &st0) == -1) {
    LOGE("Failed to get client fd stats\n");

    close(fd);
    free(args);

    pthread_exit(NULL);
  }

  module_entry(fd);

  struct stat st1;
  if (fstat(fd, &st1) == -1) {
    LOGE("Failed to get client fd stats\n");

    close(fd);
    free(args);

    pthread_exit(NULL);
  }

  if (st0.st_dev != st1.st_dev || st0.st_ino != st1.st_ino) {
    LOGI("Client fd changed. Closing.\n");

    close(fd);
  }

  free(args);

  return NULL;
}

/* WARNING: Dynamic memory based */
void companion_entry(int fd) {
  LOGI("New companion entry.\n - Client fd: %d\n", fd);

  /* TODO: Use non-NULL string termination */
  char name[256 + 1];
  ssize_t name_length = read_string(fd, name, sizeof(name) - 1);
  if (name_length == -1) {
    LOGE("Failed to read module name\n");

    ssize_t ret = write_uint8_t(fd, 2);
    ASSURE_SIZE_WRITE("ZygiskdCompanion", "name", ret, sizeof(uint8_t));

    exit(0);
  }
  name[name_length] = '\0';

  LOGI(" - Module name: \"%s\"\n", name);

  int library_fd = read_fd(fd);
  ssize_t ret = 0;
  if (library_fd == -1) {
    LOGE("Failed to receive library fd\n");

    ret = write_uint8_t(fd, 2);
    ASSURE_SIZE_WRITE("ZygiskdCompanion", "library_fd", ret, sizeof(uint8_t));

    exit(0);
  }

  LOGI(" - Library fd: %d\n", library_fd);

  zygisk_companion_entry_func module_entry = load_module(library_fd);
  close(library_fd);

  if (module_entry == NULL) {
    LOGE("No companion module entry for module: %s\n", name);

    ret = write_uint8_t(fd, 0);
    ASSURE_SIZE_WRITE("ZygiskdCompanion", "module_entry", ret, sizeof(uint8_t));

    exit(0);
  } else {
    ret = write_uint8_t(fd, 1);
    ASSURE_SIZE_WRITE("ZygiskdCompanion", "module_entry", ret, sizeof(uint8_t));
  }

  while (1) {
    if (!check_unix_socket(fd, true)) {
      LOGE("Something went wrong in companion. Bye!\n");

      exit(0);

      break;
    }
  
    int client_fd = read_fd(fd);
    if (fd == -1) {
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

    LOGI("New companion request.\n - Module name: %s\n - Client fd: %d\n", name, args->fd);

    ret = write_uint8_t(args->fd, 1);
    ASSURE_SIZE_WRITE("ZygiskdCompanion", "client_fd", ret, sizeof(uint8_t));
    
    pthread_t thread;
    pthread_create(&thread, NULL, entry_thread, args);
  }
}
