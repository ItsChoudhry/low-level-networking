#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BACKLOG 10
#define BUFSZ 4096

static volatile sig_atomic_t running = 1; // 1 = running, 0 = stop
static int sigpipe_fds[2] = {-1, -1};     // [0]=read end, [1]=write end

static void on_signal(int signo) {
  (void)signo;
  running = 0;

  // wake select(): write one byte (async-signal-safe)
  if (sigpipe_fds[1] != -1) {
    const uint8_t b = 1;
    (void)write(sigpipe_fds[1], &b, 1);
  }
}

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    return -1;
  return 0;
}

static ssize_t sendall(int fd, const void *buf, size_t len) {
  const char *p = buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, p + sent, len - sent, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    sent += (size_t)n;
  }

  return (ssize_t)sent;
}

static void rstrip_crlf(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
}

struct client_state {
  int fd;
  char linebuf[BUFSZ * 2];
  size_t line_len;
  char who[64];
};

static struct client_state clients[FD_SETSIZE];

static void clients_init(void) {
  for (int i = 0; i < FD_SETSIZE; ++i) {
    clients[i].fd = -1;
    clients[i].line_len = 0;
    clients[i].who[0] = '\0';
  }
}

static void broadcast_line(fd_set *master, int fdmax, const char *msg, int from_fd,
                           int include_sender) {
  for (int fd = 0; fd <= fdmax; ++fd) {
    // skip non-clients
    if (clients[fd].fd == -1)
      continue;

    // optionally skip the sender
    if (!include_sender && fd == from_fd)
      continue;

    if (sendall(fd, msg, strlen(msg)) < 0) {
      perror("send (broadcast)");
      // if a send fails, close and remove this client
      close(fd);
      FD_CLR(fd, master);
      clients[fd].fd = -1;
      clients[fd].line_len = 0;
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    return 1;
  }
  const char *port = argv[1];
  struct addrinfo hints, *res, *p;

  int sockfd, status, yes = 1;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  status = getaddrinfo(NULL, port, &hints, &res);
  if (status != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
    return 1;
  }

  for (p = res; p != NULL; p = p->ai_next) {
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1) {
      perror("socket");
      continue;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == -1) {
      perror("setsockopt");
      close(sockfd);
      continue;
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      perror("bind");
      close(sockfd);
      sockfd = -1;
      continue;
    }

    break;
  }

  if (p == NULL) {
    fprintf(stderr, "failed to bind to any address\n");
    freeaddrinfo(res);
    return 2;
  }

  freeaddrinfo(res);

  if (listen(sockfd, BACKLOG) == -1) {
    perror("listen");
    return 1;
  }

  if (set_nonblock(sockfd) == -1) {
    perror("set_nonblock");
    return 1;
  }

  printf("server: Listening on port %s \n", argv[1]);

  // create self-pipe
  if (pipe(sigpipe_fds) == -1) {
    perror("pipe");
    return 1;
  }

  // make pipe nonblocking so repeated writes/reads never stall
  fcntl(sigpipe_fds[0], F_SETFL, O_NONBLOCK);
  fcntl(sigpipe_fds[1], F_SETFL, O_NONBLOCK);

  // install signal handlers
  struct sigaction sa = {0};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, NULL) == -1) {
    perror("sigaction SIGINT");
  }
  if (sigaction(SIGTERM, &sa, NULL) == -1) {
    perror("sigaction SIGTERM");
  }

  clients_init();

  fd_set master, readfds;
  FD_ZERO(&master);
  FD_SET(sockfd, &master); // watch the listening socket
  FD_SET(sigpipe_fds[0], &master);
  int fdmax = sockfd; // highest-numbered fd we know about
  if (sigpipe_fds[0] > fdmax)
    fdmax = sigpipe_fds[0];

  while (1) {
    readfds = master;
    int nready = select(fdmax + 1, &readfds, NULL, NULL, NULL);
    if (nready < 0) {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }

    // Did a signal wake us?
    if (FD_ISSET(sigpipe_fds[0], &readfds)) {
      // drain the pipe
      uint8_t buf[64];
      while (read(sigpipe_fds[0], buf, sizeof buf) > 0) {
      }
      --nready;

      if (!running) {
        // fall out of the loop to clean up
        break;
      }
    }

    // if sockfd is readable? as in is there a client pending
    if (FD_ISSET(sockfd, &readfds)) {
      struct sockaddr_storage ss;
      socklen_t slen = sizeof ss;
      int new_fd = accept(sockfd, (struct sockaddr *)&ss, &slen);
      if (new_fd != -1) {
        if (set_nonblock(new_fd) == -1) {
          perror("set_nonblock(new_fd)");
          close(new_fd);
          continue;
        }
        clients[new_fd].fd = new_fd;
        clients[new_fd].line_len = 0;

        FD_SET(new_fd, &master);
        if (new_fd > fdmax)
          fdmax = new_fd;

        void *addr = (ss.ss_family == AF_INET) ? (void *)&((struct sockaddr_in *)&ss)->sin_addr
                                               : (void *)&((struct sockaddr_in6 *)&ss)->sin6_addr;
        inet_ntop(ss.ss_family, addr, clients[new_fd].who, sizeof clients[new_fd].who);

        const char *banner = "Welcome!\n";
        sendall(new_fd, banner, strlen(banner));
      }
      if (--nready == 0) {
        continue;
      }
    }

    for (int fd = 0; fd <= fdmax && nready > 0; ++fd) {
      if (fd == sockfd)
        continue;
      if (fd == sigpipe_fds[0] || fd == sigpipe_fds[1])
        continue;
      if (clients[fd].fd == -1)
        continue;
      if (!FD_ISSET(fd, &readfds))
        continue;
      --nready;

      char inbuf[BUFSZ];
      ssize_t r = recv(fd, inbuf, sizeof inbuf, 0);

      if (r == 0) {
        close(fd);
        FD_CLR(fd, &master);
        clients[fd].fd = -1;
        clients[fd].line_len = 0;
        continue;
      }

      if (r < 0) {
        if (errno == EINTR)
          continue;
        perror("recv");
        close(fd);
        FD_CLR(fd, &master);
        clients[fd].fd = -1;
        clients[fd].line_len = 0;
        continue;
      }

      size_t off = 0;
      while (off < (size_t)r) {
        if (clients[fd].line_len < sizeof(clients[fd].linebuf) - 1) {
          clients[fd].linebuf[clients[fd].line_len++] = inbuf[off++];
          clients[fd].linebuf[clients[fd].line_len] = '\0';
        } else {
          // too long: notify & skip to next newline
          const char *msg = "error: line too long\n";
          if (sendall(fd, msg, strlen(msg)) < 0)
            perror("send");
          clients[fd].line_len = 0;
          // skip remaining bytes until newline
          while (off < (size_t)r && inbuf[off++] != '\n') {
          }
        }

        if (clients[fd].line_len && clients[fd].linebuf[clients[fd].line_len - 1] == '\n') {
          rstrip_crlf(clients[fd].linebuf);

          printf("%s (fd=%d): \"%s\"\n", clients[fd].who, fd, clients[fd].linebuf);
          fflush(stdout);

          char out[BUFSZ + 128];
          int m = snprintf(out, sizeof out, "%s: %s\n", clients[fd].who, clients[fd].linebuf);
          if (m > 0) {
            broadcast_line(&master, fdmax, out, fd, 1);
          }
          clients[fd].line_len = 0; // ready for next line
        }
      }
    }
  }

  for (int fd = 0; fd <= fdmax; ++fd) {
    if (fd == sigpipe_fds[0] || fd == sigpipe_fds[1])
      continue;
    if (fd == sockfd)
      continue;
    if (clients[fd].fd != -1) {
      close(fd);
      clients[fd].fd = -1;
      clients[fd].line_len = 0;
      clients[fd].who[0] = '\0';
    }
  }

  if (sockfd >= 0)
    close(sockfd);
  if (sigpipe_fds[0] != -1)
    close(sigpipe_fds[0]);
  if (sigpipe_fds[1] != -1)
    close(sigpipe_fds[1]);
  fprintf(stderr, "server: shut down gracefully.\n");
  return 0;
}
