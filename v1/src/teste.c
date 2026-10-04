#include <stdio.h>
#include <pthread.h>

void *tarefa(void *arg) {
    printf("Olá da thread %ld\n", (long)arg);
    return NULL;
}

int main(void) {
    pthread_t t[3];
    for (long i = 0; i < 3; i++)
        pthread_create(&t[i], NULL, tarefa, (void *)i);
    for (int i = 0; i < 3; i++)
        pthread_join(t[i], NULL);
    return 0;
}