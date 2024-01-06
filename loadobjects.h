#ifndef loadobjects_h
#define loadobjects_h

typedef struct server{
    int32_t req_count;
    int32_t err_count;
    uint16_t port;
    bool responsive;
}server;

typedef struct ThreadSafeQueue {
  int front, rear, size, capacity;
  int **array;
  pthread_mutex_t mutex;
} ThreadSafeQueue;

ThreadSafeQueue *create_queue(int capacity) {
  ThreadSafeQueue *q = (ThreadSafeQueue *) malloc(sizeof(ThreadSafeQueue));
  pthread_mutex_init(&q->mutex, NULL);
  q->capacity = capacity;
  q->front = 0;
  q->size = 0;
  q->rear = capacity - 1;
  q->array = calloc(q->capacity, sizeof(int *));
  return q;
}

bool enqueue(ThreadSafeQueue *q, int *item) {
  pthread_mutex_lock(&q->mutex);
  if (q->size == q->capacity) {
    pthread_mutex_unlock(&q->mutex);
    return false;
  }
  q->size = q->size + 1;
  q->rear = (q->rear + 1) % q->capacity;
  q->array[q->rear] = item;
  pthread_mutex_unlock(&q->mutex);
  return true;
}

int *dequeue(ThreadSafeQueue *q) {
  pthread_mutex_lock(&q->mutex);
  if (q->size == 0) {
    pthread_mutex_unlock(&q->mutex);
    return NULL;
  }
  int *item = q->array[q->front];
  q->front = (q->front + 1) % q->capacity;
  q->size = q->size - 1;
  pthread_mutex_unlock(&q->mutex);
  return item;
}

#endif
