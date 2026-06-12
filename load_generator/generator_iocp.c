#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9090
#define DEFAULT_CLIENTS 50
#define DEFAULT_MESSAGES 20
#define DEFAULT_DELAY_US 200000

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
    static LARGE_INTEGER frequency;
    static int initialized = 0;

    LARGE_INTEGER counter;

    if (!initialized)
    {
        QueryPerformanceFrequency(&frequency);
        initialized = 1;
    }

    QueryPerformanceCounter(&counter);

    return
        (double)counter.QuadPart /
        (double)frequency.QuadPart;
}

SOCKET connect_to_server(const char *host, int port)
{
    SOCKET fd =
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (fd == INVALID_SOCKET)
    {
        printf(
            "socket failed: %d\n",
            WSAGetLastError()
        );
        return INVALID_SOCKET;
    }

    struct sockaddr_in server_addr;

    memset(
        &server_addr,
        0,
        sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (
        inet_pton(
            AF_INET,
            host,
            &server_addr.sin_addr) <= 0)
    {
        closesocket(fd);
        return INVALID_SOCKET;
    }

    if (
        connect(
            fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr))
        == SOCKET_ERROR)
    {
        closesocket(fd);
        return INVALID_SOCKET;
    }

    return fd;
}

DWORD WINAPI client_thread(LPVOID arg)
{
    ClientConfig *cfg = (ClientConfig *)arg;

    SOCKET fd =
    connect_to_server(
        cfg->host,
        cfg->port);
    if (fd == INVALID_SOCKET)
    {
        printf("[Client %d] failed to connect\n", cfg->id);
        free(cfg);
        return 0;
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

        int sent = send(fd, send_buffer, strlen(send_buffer), 0);
        if (sent < 0)
        {
            perror("send");
            break;
        }

        memset(recv_buffer, 0, sizeof(recv_buffer));

        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);

        int received =
            recv(
                fd,
                recv_buffer,
                sizeof(recv_buffer) - 1,
                0);
        if (received > 0)
        {
            recv_buffer[received] = '\0';
            printf("[Client %d] received: %s", cfg->id, recv_buffer);
        }
        else if (received == SOCKET_ERROR)
        {
            int err = WSAGetLastError();

            if (err != WSAEWOULDBLOCK)
            {
                printf("recv failed: %d\n", err);
                break;
            }
        }

        Sleep(cfg->delay_us / 1000);
    }
    Sleep(5000);
    closesocket(fd);
    printf("[Client %d] disconnected\n", cfg->id);

    free(cfg);
    return 0;
}

int main(int argc, char *argv[])
{
    WSADATA wsa;

    if (
        WSAStartup(
            MAKEWORD(2,2),
            &wsa) != 0)
    {
        printf(
            "WSAStartup failed\n");
        return 1;
    }
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

    HANDLE *threads = malloc(sizeof(HANDLE) * clients);
    if (!threads)
    {
        perror("malloc");
        return 1;
    }

    for (int i = 0; i < clients; i++)
    {
        ClientConfig *cfg =
            malloc(sizeof(ClientConfig));

        if (!cfg)
        {
            printf("malloc failed\n");
            free(threads);
            return 1;
        }

        cfg->id = i + 1;
        cfg->host = host;
        cfg->port = port;
        cfg->messages = messages;
        cfg->delay_us = delay_us;

        threads[i] =
            CreateThread(
                NULL,
                0,
                client_thread,
                cfg,
                0,
                NULL);

        if (threads[i] == NULL)
        {
            printf("CreateThread failed\n");
            free(cfg);
        }
    }
    printf("Waiting for threads...\n");
    for(int i = 0; i < clients; i++)
    {
        WaitForSingleObject(
            threads[i],
            INFINITE);
    }
    printf("Threads finished\n");
    for(int i = 0; i < clients; i++)
    {
        CloseHandle(
            threads[i]);
    }

    free(threads);

    printf("Load test complete\n");
    WSACleanup();
    return 0;
}