#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // malloc(), free(), atoi()
#include <string.h>     // memset(), snprintf(), strlen()
#include <unistd.h>     // close(), usleep()
#include <arpa/inet.h>  // htons(), inet_pton()
#include <sys/socket.h> // socket(), connect(), send(), recv()
#include <netinet/in.h> // sockaddr_in
#include <pthread.h>    // pthread_create(), pthread_join()
#include <errno.h>      // errno
#include <time.h>       // clock_gettime()

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9090
#define DEFAULT_CLIENTS 50
#define DEFAULT_MESSAGES 20
#define DEFAULT_DELAY_US 200000   // 200 ms

typedef struct
{
    int id;
    const char *host;
    int port;
    int messages;
    int delay_us;
} ClientConfig;

/* Monotonic time is safer for latency measurements. */
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int connect_to_server(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

void *client_thread(void *arg)
{
    ClientConfig *cfg = (ClientConfig *)arg;

    int fd = connect_to_server(cfg->host, cfg->port);
    if (fd < 0)
    {
        printf("[Client %d] failed to connect\n", cfg->id);
        free(cfg);
        return NULL;
    }

    printf("[Client %d] connected\n", cfg->id);

    char send_buffer[256];
    char recv_buffer[1024];

    for (int i = 0; i < cfg->messages; i++)
    {
        double send_time = now_seconds();

        snprintf(send_buffer,
                 sizeof(send_buffer),
                 "%d,%d,%.6f\n",
                 cfg->id,
                 i + 1,
                 send_time);

        ssize_t sent = send(fd, send_buffer, strlen(send_buffer), 0);
        if (sent < 0)
        {
            perror("send");
            break;
        }

        memset(recv_buffer, 0, sizeof(recv_buffer));

        ssize_t received = recv(fd, recv_buffer, sizeof(recv_buffer) - 1, MSG_DONTWAIT);
        if (received > 0)
        {
            recv_buffer[received] = '\0';
            printf("[Client %d] received: %s", cfg->id, recv_buffer);
        }
        else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            perror("recv");
            break;
        }

        usleep(cfg->delay_us);
    }
    sleep(5);
    close(fd);
    printf("[Client %d] disconnected\n", cfg->id);

    free(cfg);
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    int clients = DEFAULT_CLIENTS;
    int messages = DEFAULT_MESSAGES;
    int delay_us = DEFAULT_DELAY_US;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);
    if (argc > 3) clients = atoi(argv[3]);
    if (argc > 4) messages = atoi(argv[4]);
    if (argc > 5) delay_us = atoi(argv[5]);

    printf("Load generator starting\n");
    printf("Host: %s\n", host);
    printf("Port: %d\n", port);
    printf("Clients: %d\n", clients);
    printf("Messages per client: %d\n", messages);
    printf("Delay per message: %d us\n", delay_us);

    pthread_t *threads = malloc(sizeof(pthread_t) * clients);
    if (!threads)
    {
        perror("malloc");
        return 1;
    }

    for (int i = 0; i < clients; i++)
    {
        ClientConfig *cfg = malloc(sizeof(ClientConfig));
        if (!cfg)
        {
            perror("malloc");
            free(threads);
            return 1;
        }

        cfg->id = i + 1;
        cfg->host = host;
        cfg->port = port;
        cfg->messages = messages;
        cfg->delay_us = delay_us;

        if (pthread_create(&threads[i], NULL, client_thread, cfg) != 0)
        {
            perror("pthread_create");
            free(cfg);
            continue;
        }
    }

    for (int i = 0; i < clients; i++)
    {
        pthread_join(threads[i], NULL);
    }

    free(threads);

    printf("Load test complete\n");
    return 0;
}