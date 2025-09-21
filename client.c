#include <stddef.h>
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
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

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <hostname> <port> [message]\n", argv[0]);
    return 1;
  }

  const char *host = argv[1];
  const char *port = argv[2];
  const char *msg = argv[3];

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

  size_t len = strlen(msg);
  if (send(sockfd, msg, len, 0) != (ssize_t)len) {
    perror("send");
    close(sockfd);
    return 1;
  }

  char buf[BUFSZ];
  ssize_t n = recv(sockfd, buf, sizeof buf - 1, 0);
  if (n < 0) {
    perror("recv");
    close(sockfd);
    return 1;
  }

  buf[n] = '\0';

  printf("\"%s\"\n", buf);

  close(sockfd);

  return 0;
}
