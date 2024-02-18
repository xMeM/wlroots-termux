#ifndef UTIL_SHM_H
#define UTIL_SHM_H

#include <stdbool.h>

#ifdef __ANDROID__
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static inline int shm_unlink(const char *name) {
    size_t namelen;
    char fname[4095];

    /* Construct the filename.  */
    while (name[0] == '/') ++name;

    if (name[0] == '\0') {
        /* The name "/" is not supported.  */
        errno = EINVAL;
        return -1;
    }

    namelen = strlen(name);
    memcpy(fname, "/data/data/com.termux/files/usr/tmp/", sizeof("/data/data/com.termux/files/usr/tmp/") - 1);
    memcpy(fname + sizeof("/data/data/com.termux/files/usr/tmp/") - 1, name, namelen + 1);

    return unlink(fname);
}

static inline int shm_open(const char *name, int oflag, mode_t mode) {
    size_t namelen;
    char fname[4095];
    int fd;

    /* Construct the filename.  */
    while (name[0] == '/') ++name;

    if (name[0] == '\0') {
        /* The name "/" is not supported.  */
        errno = EINVAL;
        return -1;
    }

    namelen = strlen(name);
    memcpy(fname, "/data/data/com.termux/files/usr/tmp/", sizeof("/data/data/com.termux/files/usr/tmp/") - 1);
    memcpy(fname + sizeof("/data/data/com.termux/files/usr/tmp/") - 1, name, namelen + 1);

    fd = open(fname, oflag, mode);
    if (fd != -1) {
        /* We got a descriptor.  Now set the FD_CLOEXEC bit.  */
        int flags = fcntl(fd, F_GETFD, 0);
        flags |= FD_CLOEXEC;
        flags = fcntl(fd, F_SETFD, flags);

        if (flags == -1) {
            /* Something went wrong.  We cannot return the descriptor.  */
            int save_errno = errno;
            close(fd);
            fd = -1;
            errno = save_errno;
        }
    }

    return fd;
}
#endif

int create_shm_file(void);
int allocate_shm_file(size_t size);
bool allocate_shm_file_pair(size_t size, int *rw_fd, int *ro_fd);

#endif
