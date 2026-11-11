#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

typedef ssize_t (*send_fn)(int, const void *, size_t, int);
typedef ssize_t (*sendto_fn)(int, const void *, size_t, int,
                             const struct sockaddr *, socklen_t);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef ssize_t (*sendmsg_fn)(int, const struct msghdr *, int);

static send_fn real_send;
static sendto_fn real_sendto;
static write_fn real_write;
static sendmsg_fn real_sendmsg;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread int resolving_symbol;
static __thread int inside_logger;

#define DEFINE_RESOLVER(name, type)                                      \
  static type get_real_##name(void) {                                    \
    type function = __atomic_load_n(&real_##name, __ATOMIC_ACQUIRE);      \
    if (function != NULL) {                                               \
      return function;                                                   \
    }                                                                    \
    if (resolving_symbol) {                                              \
      return NULL;                                                       \
    }                                                                    \
    resolving_symbol = 1;                                                \
    int saved_errno = errno;                                             \
    void *symbol = dlsym(RTLD_NEXT, #name);                              \
    errno = saved_errno;                                                 \
    resolving_symbol = 0;                                                \
    if (symbol != NULL) {                                                \
      type candidate = (type)symbol;                                     \
      type expected = NULL;                                              \
      __atomic_compare_exchange_n(&real_##name, &expected, candidate, 0, \
                                  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);    \
      function = __atomic_load_n(&real_##name, __ATOMIC_ACQUIRE);         \
    }                                                                    \
    return function;                                                     \
  }

DEFINE_RESOLVER(send, send_fn)
DEFINE_RESOLVER(sendto, sendto_fn)
DEFINE_RESOLVER(write, write_fn)
DEFINE_RESOLVER(sendmsg, sendmsg_fn)

static int is_socket_fd(int fd) {
  int saved_errno = errno;
  int socket_type = 0;
  socklen_t length = sizeof(socket_type);
  int result = getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type, &length);
  errno = saved_errno;
  return result == 0;
}

static void write_all_with_syscall(int fd, const char *data, size_t length) {
  while (length != 0) {
    ssize_t written = (ssize_t)syscall(SYS_write, fd, data, length);
    if (written > 0) {
      data += written;
      length -= (size_t)written;
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
}

static void write_hex_with_syscall(int fd, const unsigned char *data,
                                   size_t length) {
  static const char digits[] = "0123456789abcdef";
  char buffer[2048];

  while (length != 0) {
    size_t count = length;
    if (count > sizeof(buffer) / 2) {
      count = sizeof(buffer) / 2;
    }

    for (size_t i = 0; i < count; ++i) {
      buffer[i * 2] = digits[data[i] >> 4];
      buffer[i * 2 + 1] = digits[data[i] & 0x0f];
    }

    write_all_with_syscall(fd, buffer, count * 2);
    data += count;
    length -= count;
  }
}

static int begin_log_line(int socket_fd, size_t length, int *log_fd) {
  const char *path = getenv("SENDLOG_FILE");
  if (path == NULL || path[0] == '\0') {
    return 0;
  }

  char prefix[128];
  int prefix_length =
      snprintf(prefix, sizeof(prefix), "%d %zu ", socket_fd, length);
  if (prefix_length <= 0 || (size_t)prefix_length >= sizeof(prefix)) {
    return 0;
  }

  pthread_mutex_lock(&log_mutex);
  int fd = (int)syscall(SYS_openat, AT_FDCWD, path,
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) {
    pthread_mutex_unlock(&log_mutex);
    return 0;
  }

  write_all_with_syscall(fd, prefix, (size_t)prefix_length);
  *log_fd = fd;
  return 1;
}

static void finish_log_line(int log_fd) {
  write_all_with_syscall(log_fd, "\n", 1);
  syscall(SYS_close, log_fd);
  pthread_mutex_unlock(&log_mutex);
}

static void log_bytes(int socket_fd, const void *bytes, size_t length) {
  if (length == 0 || inside_logger || resolving_symbol) {
    return;
  }

  int saved_errno = errno;
  inside_logger = 1;

  int log_fd;
  if (begin_log_line(socket_fd, length, &log_fd)) {
    write_hex_with_syscall(log_fd, (const unsigned char *)bytes, length);
    finish_log_line(log_fd);
  }

  inside_logger = 0;
  errno = saved_errno;
}

static void log_iovecs(int socket_fd, const struct iovec *iov,
                       size_t iov_count, size_t total_length) {
  if (total_length == 0 || inside_logger || resolving_symbol) {
    return;
  }

  int saved_errno = errno;
  inside_logger = 1;

  int log_fd;
  if (begin_log_line(socket_fd, total_length, &log_fd)) {
    for (size_t i = 0; i < iov_count; ++i) {
      if (iov[i].iov_len != 0) {
        write_hex_with_syscall(log_fd,
                               (const unsigned char *)iov[i].iov_base,
                               iov[i].iov_len);
      }
    }
    finish_log_line(log_fd);
  }

  inside_logger = 0;
  errno = saved_errno;
}

ssize_t send(int socket_fd, const void *buffer, size_t length, int flags) {
  int saved_errno = errno;
  send_fn function = get_real_send();
  errno = saved_errno;

  if (!resolving_symbol && is_socket_fd(socket_fd)) {
    log_bytes(socket_fd, buffer, length);
  }
  errno = saved_errno;

  if (function != NULL) {
    return function(socket_fd, buffer, length, flags);
  }
  return (ssize_t)syscall(SYS_sendto, socket_fd, buffer, length, flags, NULL,
                          0);
}

ssize_t sendto(int socket_fd, const void *buffer, size_t length, int flags,
               const struct sockaddr *address, socklen_t address_length) {
  int saved_errno = errno;
  sendto_fn function = get_real_sendto();
  errno = saved_errno;

  if (!resolving_symbol && is_socket_fd(socket_fd)) {
    log_bytes(socket_fd, buffer, length);
  }
  errno = saved_errno;

  if (function != NULL) {
    return function(socket_fd, buffer, length, flags, address, address_length);
  }
  return (ssize_t)syscall(SYS_sendto, socket_fd, buffer, length, flags,
                          address, address_length);
}

ssize_t write(int fd, const void *buffer, size_t length) {
  int saved_errno = errno;
  write_fn function = get_real_write();
  errno = saved_errno;

  if (!resolving_symbol && is_socket_fd(fd)) {
    log_bytes(fd, buffer, length);
  }
  errno = saved_errno;

  if (function != NULL) {
    return function(fd, buffer, length);
  }
  return (ssize_t)syscall(SYS_write, fd, buffer, length);
}

ssize_t sendmsg(int socket_fd, const struct msghdr *message, int flags) {
  int saved_errno = errno;
  sendmsg_fn function = get_real_sendmsg();
  errno = saved_errno;

  if (!resolving_symbol && message != NULL && is_socket_fd(socket_fd)) {
    size_t total_length = 0;
    int valid_length = 1;

    for (size_t i = 0; i < message->msg_iovlen; ++i) {
      if (message->msg_iov[i].iov_len > SIZE_MAX - total_length) {
        valid_length = 0;
        break;
      }
      total_length += message->msg_iov[i].iov_len;
    }

    if (valid_length && total_length != 0) {
      log_iovecs(socket_fd, message->msg_iov, message->msg_iovlen,
                 total_length);
    }
  }
  errno = saved_errno;

  if (function != NULL) {
    return function(socket_fd, message, flags);
  }
  return (ssize_t)syscall(SYS_sendmsg, socket_fd, message, flags);
}
