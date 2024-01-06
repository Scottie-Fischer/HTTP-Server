#include<err.h>
#include<arpa/inet.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <pthread.h>
#include "starter.h"
#include "loadobjects.h"



int N = 4, R = 5, server_count;

int REQUEST_COUNT = 0;
int timer = 3;

struct server *servers;
pthread_t *threads;
ThreadSafeQueue *queue;
pthread_cond_t conditionVar = PTHREAD_COND_INITIALIZER;
pthread_cond_t health_check_condVar = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t health_mut = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t server_mut = PTHREAD_MUTEX_INITIALIZER;
//--------------------------------------------------------------------------------------------------------------------------//
void parse_args(int argc, char **argv, uint16_t *port) {
    if (argc < 3) {
        fprintf(stderr, "missing arguments: usage %s port_to_connect port_to_listen", argv[0]);
        exit(1);
    }
    //int opt;
    int opt_args_count = 0, option;
    while ((option = getopt(argc, argv, "N:R:")) != -1) {
        switch(option){
            case 'N':
                if(optarg == NULL){
                    perror("optarg is null");
                    exit(1);
                }
                if(sscanf(optarg,"%d",N) != 1){
                    perror("not integer");
                    exit(1);
                }
                if(N <= 0) {
                    perror("Arguments must be non-negative");
                    exit(1);
                }
                opt_args_count++;
                //printf("Done Checking N\n");
                break;
            case 'R':
                if(optarg == NULL){
                    perror("optarg is NULL");
                    exit(1);
                }
                if(strlen(optarg) <= 0){
                    perror("optarg is nothing");
                    exit(1);
                }
                for(int i = 0; i < strlen(optarg); i ++){
                    if(isdigit(optarg[i]) <= 0) {
                        perror("Args are not proper numbers");
                        exit(1);
                    }
                }
                if(atoi(optarg) < 0){
                    perror("Arguments must be non-negative");
                    exit(1);
                }
                R = atoi(optarg);
                opt_args_count++;
                break;
            case '?':
                perror("Flags require arguments");
                exit(1);
        }
    }

    initialize_servers(argc, argv, port, opt_args_count);
}

//----------------------------------------------------------------------------------------------------------------//
bool parse_num(char *buffer, int *num) {
    return sscanf(buffer, "%d", num) == 1;
}

void initialize_servers(int argc, char *const *argv, uint16_t *port, int opt_args_count) {
  bool isFirstNum = true;
  server_count = argc - 1 - (2 * opt_args_count) - 1;
  servers = calloc(sizeof(server), server_count);
  int j = 0, num;
  for (int i = 1; i < argc; ++i) {
    if (parse_num(argv[i], &num)) {
      if (isFirstNum) {
        *port = num;
        isFirstNum = false;
      } else {
        servers[j++].port = num;
      }
    } else {
      i++; // skip optional arg, and it's value
    }
  }
}

server choose_server(server s1, server s2) {
  if (!s1.responsive) return s2;
  if (!s2.responsive) return s1;

  if (s1.req_count > s2.req_count) {
    return s2;
  } else if (s2.req_count > s1.req_count) {
    return s1;
  }
  const int32_t s1_succ_rate = 1 - (s1.err_count / s1.req_count);
  const int32_t s2_succ_rate = 1 - (s2.err_count / s2.req_count);
  return s1_succ_rate > s2_succ_rate ? s1 : s2;
}

server find_best_server() {
  server best_server = servers[0];
  for (int i = 1; i < server_count; ++i) {
    best_server = choose_server(best_server, servers[i]);
  }
  return best_server;
}

void handle_request(int acceptfd) {
  int connfd;

  pthread_mutex_lock(&server_mut);
  server server = find_best_server();
  if (server.responsive) {
    server.req_count++;
  }
  pthread_mutex_unlock(&server_mut);

  if ((connfd = client_connect(server.port)) < 0) {
    handle_unresponsive(acceptfd);
    return;
  }

  bridge_loop(acceptfd, connfd);
  close(connfd);
}

_Noreturn void *thread_handle_request() {
  while (true) {
    int *socket = dequeue(queue);

    if (socket == NULL) {
      pthread_mutex_lock(&mut);
      pthread_cond_wait(&conditionVar, &mut);
      pthread_mutex_unlock(&mut);
      socket = dequeue(queue);
    }

    if (socket) {
      handle_request(*socket);
      free(socket);
    }
  }
}

void init_threads(int thread_count) {
  threads = calloc(thread_count, sizeof(pthread_t));
  for (int i = 0; i < thread_count; ++i)
    pthread_create(&threads[i], NULL, thread_handle_request, NULL);
}

void call_health_check() {
//  printf("Called\n");
  pthread_cond_signal(&health_check_condVar);
}

void inc_request_count() {
  pthread_mutex_lock(&health_mut);
  REQUEST_COUNT++;
  pthread_mutex_unlock(&health_mut);
  if (REQUEST_COUNT % R == 0) call_health_check();
}

void send_health_check(server *server) {
  int connfd;
  char recv[200];
  ssize_t read_ret;
  const char *health_check_req = "GET /healthcheck HTTP/1.1\r\n\r\n";

  if (((connfd = client_connect(server->port)) < 0) ||
      (send(connfd, health_check_req, strlen(health_check_req), 0) < 0) ||
      ((read_ret = read(connfd, recv, 200)) < -1)) {
    server->responsive = false;
    close(connfd);
    return;
  }

  recv[read_ret] = '\0';
  int status_code;
  if (sscanf(recv, "HTTP/1.1 %d OK\r\n", &status_code) != 1 && status_code != 200) {
    server->responsive = false;
  } else {
    char *body = strstr(recv, "\r\n\r\n");
    server->responsive = sscanf(body, "%d\n%d", &server->err_count, &server->req_count) == 2
                         && server->req_count >= server->err_count;
  }
  close(connfd);
}

_Noreturn void *health_check_probe() {
  while (true) {
    pthread_mutex_lock(&health_mut);
    pthread_cond_wait(&health_check_condVar, &health_mut);
    pthread_mutex_unlock(&health_mut);

    for (int i = 0; i < server_count; ++i)
      send_health_check(&servers[i]);
  }
}

_Noreturn void *health_check_timer() {
  while (true) {
    sleep(timer);
    call_health_check();
  }
}

int main(int argc, char **argv) {
  int listenfd, acceptfd;
  uint16_t listenport;
  parse_args(argc, argv, &listenport);
  queue = create_queue(4 * N);
  init_threads(N);

  pthread_t pt;
  pthread_create(&pt, NULL, health_check_probe, NULL);
  pthread_cond_signal(&health_check_condVar);

  pthread_t pt_timer;
  pthread_create(&pt_timer, NULL, health_check_timer, NULL);

  if ((listenfd = server_listen(listenport)) < 0)
    err(1, "failed listening");

  while (true) {
    if ((acceptfd = accept(listenfd, NULL, NULL)) < 0) {
      err(1, "failed accepting");
    }

    // store socket fd as a int pointer
    int *sockd = malloc(sizeof(acceptfd));
    *sockd = acceptfd;
    // Add request socket to queue
    enqueue(queue, sockd);
    inc_request_count();
    // notify threads are there are new requests to handle
    pthread_cond_signal(&conditionVar);
  }
}