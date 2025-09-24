#include <stddef.h>
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFSZ 4096

static void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in *)sa)->sin_addr);
  }
  return &(((struct sockaddr_in6 *)sa)->sin6_addr);
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

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <hostname> <port> \n", argv[0]);
    return 1;
  }

  const char *host = argv[1];
  const char *port = argv[2];

  struct addrinfo hints, *res, *p;
  char s[INET6_ADDRSTRLEN];

  int status, sockfd = -1;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  status = getaddrinfo(host, port, &hints, &res);
  if (status != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
    return 2;
  }

  for (p = res; res != NULL; res = res->ai_next) {
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1) {
      perror("client: socket");
      continue;
    }

    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);
    printf("client: attempting to connect to %s\n", s);

    if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      perror("client: connect");
      close(sockfd);
      sockfd = -1;
      continue;
    }

    break;
  }

  if (p == NULL) {
    fprintf(stderr, "client: failed to connect\n");
    freeaddrinfo(res);
    return 2;
  }

  memset(s, 0, sizeof s);
  inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);
  printf("client: connected to %s\n", s);

  freeaddrinfo(res);

  char in[4096];  // stdin
  char out[4096]; // socket

  while (1) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);                         // monitor stdin
    FD_SET(sockfd, &rfds);                    // monitor socket
    int nfds = (sockfd > 0 ? sockfd : 0) + 1; // highest-numbered fd we know about

    int n = select(nfds, &rfds, NULL, NULL, NULL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }

    // typed something
    if (FD_ISSET(0, &rfds)) {
      if (!fgets(in, sizeof in, stdin)) {
        break;
      }
      size_t len = strlen(in);
      if (sendall(sockfd, in, len) < 0) {
        perror("send");
        break;
      }
    }

    // server sent something
    if (FD_ISSET(sockfd, &rfds)) {
      ssize_t r = recv(sockfd, out, sizeof out - 1, 0);
      if (r == 0) {
        fprintf(stderr, "(server closed)\n");
        break;
      }
      if (r < 0) {
        if (errno == EINTR)
          continue;
        perror("recv");
        break;
      }
      out[r] = '\0';
      fputs(out, stdout);
      fflush(stdout);
    }
  }

  close(sockfd);

  return 0;
}
