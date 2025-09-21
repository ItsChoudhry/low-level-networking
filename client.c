#include <stddef.h>
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static ssize_t recvline(int fd, char *buf, size_t cap) {
  size_t used = 0;
  while (used + 1 < cap) {
    char c;
    ssize_t n = recv(fd, &c, 1, 0);
    if (n == 0) { // server closed
      if (used == 0)
        return 0; // no data read
      break;      // return what we have
    }
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    buf[used++] = c;
    if (c == '\n')
      break; // stop at end of line
  }
  buf[used] = '\0';
  return (ssize_t)used;
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

  char line[4096];
  char reply[4096];

  while (1) {
    // read a line from stdin
    if (!fgets(line, sizeof line, stdin)) {
      break;
    }

    // send that while line to server
    size_t len = strlen(line);
    if (sendall(sockfd, line, len) < 0) {
      perror("send");
      break;
    }

    // read exactly one respone line and print it
    ssize_t r = recvline(sockfd, reply, sizeof reply);
    if (r == 0) {
      printf("server closed\n");
      break;
    }
    if (r < 0) {
      perror("recv");
      break;
    }
    fputs(reply, stdout);
  }

  close(sockfd);

  return 0;
}
