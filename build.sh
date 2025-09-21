#!/usr/bin/env bash

clang -Wall -Wextra -Wpedantic -O2 -g -fsanitize=address,undefined -o server server.c
clang -Wall -Wextra -Wpedantic -O2 -g -fsanitize=address,undefined -o client client.c
