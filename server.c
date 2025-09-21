#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BACKLOG 10
#define BUFSZ 4096

static void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in *)sa)->sin_addr);
  }
  return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

static void sigchld_handler(int s) {
  (void)s;

  int saved_errno = errno;

  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;

  errno = saved_errno;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    return 1;
  }
  const char *port = argv[1];
  struct addrinfo hints, *res, *p;
  struct sockaddr_storage their_addr;
  socklen_t sin_size;
  struct sigaction sa = {0};
  char s[INET6_ADDRSTRLEN];

  int sockfd, new_fd, status, yes = 1;

  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("sigaction");
    exit(1);
  }

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  status = getaddrinfo(NULL, port, &hints, &res);
  if (status != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
    return 1;
  }

  for (p = res; res != NULL; res = res->ai_next) {
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1) {
      perror("socket");
      continue;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes)) {
      perror("setsockopt");
      close(sockfd);
      freeaddrinfo(res);
      return -1;
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

  printf("server: Listening on port %s \n", argv[1]);

  while (1) {
    sin_size = sizeof their_addr;
    new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
    if (new_fd == -1) {
      if (errno == EINTR)
        continue;
      perror("accept");
      continue;
    }

    inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), s, sizeof s);
    printf("server: geto connection from %s\n", s);

    pid_t pid = fork();
    if (pid == -1) {
      perror("for");
      close(new_fd);
      continue;
    }

    if (pid == 0) {
      close(sockfd);

      char buf[BUFSZ];
      while (1) {
        ssize_t n = recv(new_fd, buf, sizeof buf, 0);
        if (n == 0) {
          break;
        } else if (n < 0) {
          if (errno == EINTR)
            continue;
          perror("recv");
          break;
        }
        buf[n] = '\0';

        printf("server recv: %s\n", buf);

        char response[BUFSZ + 64];
        int m = snprintf(response, sizeof response, "Broadcast: %s", buf);

        if (m > 0) {
          ssize_t sent = 0;
          while (sent < m) {
            ssize_t k = send(new_fd, response + sent, (size_t)(m - sent), 0);
            if (k < 0) {
              perror("sent");
              break;
            }
            sent += k;
          }
        }
      }
      close(new_fd);
      _exit(0);
    } else {
      close(new_fd);
    }
  }
}
