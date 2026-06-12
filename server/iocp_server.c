#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib")

#define SERVER_PORT 9090
#define DEFAULT_WORKER_THREADS 4
#define MAX_CLIENTS 8192
#define RECV_CHUNK 4096
#define CLIENT_BUFFER_SIZE 65536

/*
 * Each connected client has:
 * - a socket
 * - one outstanding overlapped recv
 * - a temporary receive buffer
 * - a stream buffer for newline-framed messages
 */
typedef struct ClientContext
{
    SOCKET sock;
    OVERLAPPED recv_ov;
    WSABUF recv_wsabuf;
    char recv_buf[RECV_CHUNK];

    char msg_buf[CLIENT_BUFFER_SIZE];
    size_t msg_len;

    LONG closed;
} ClientContext;

/* Global IOCP objects and shared state */
static HANDLE g_iocp = NULL;
static SOCKET g_listen_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_client_lock;
static CRITICAL_SECTION g_log_lock;
static LARGE_INTEGER g_qpc_freq;
static volatile LONG g_running = 1;

static ClientContext* g_clients[MAX_CLIENTS];
static int g_client_count = 0;

static FILE* g_log_file = NULL;
static HANDLE* g_worker_handles = NULL;
static int g_worker_count = 0;

/* -------------------- time helpers -------------------- */

static double now_seconds(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)g_qpc_freq.QuadPart;
}

/* -------------------- logging -------------------- */

static void open_log(const char* path)
{
    g_log_file = fopen(path, "w");
    if (!g_log_file)
    {
        perror("fopen");
        exit(1);
    }

    fprintf(g_log_file, "socket_fd,receive_time,client_id,sequence,send_time,latency_ms\n");
    fflush(g_log_file);
}

static void write_record(
    SOCKET sock,
    double receive_time,
    int client_id,
    int sequence,
    double send_time,
    double latency_ms)
{
    EnterCriticalSection(&g_log_lock);

    fprintf(g_log_file,
            "%llu,%.6f,%d,%d,%.6f,%.3f\n",
            (unsigned long long)sock,
            receive_time,
            client_id,
            sequence,
            send_time,
            latency_ms);

    fflush(g_log_file);

    LeaveCriticalSection(&g_log_lock);
}

/* -------------------- client list management -------------------- */

static void add_client(ClientContext* ctx)
{
    EnterCriticalSection(&g_client_lock);

    if (g_client_count >= MAX_CLIENTS)
    {
        LeaveCriticalSection(&g_client_lock);
        InterlockedExchange(&ctx->closed, 1);
        closesocket(ctx->sock);
        free(ctx);
        return;
    }

    g_clients[g_client_count++] = ctx;

    LeaveCriticalSection(&g_client_lock);
}

static ClientContext* find_client_by_socket(SOCKET sock)
{
    ClientContext* found = NULL;

    EnterCriticalSection(&g_client_lock);
    for (int i = 0; i < g_client_count; i++)
    {
        if (g_clients[i] && g_clients[i]->sock == sock)
        {
            found = g_clients[i];
            break;
        }
    }
    LeaveCriticalSection(&g_client_lock);

    return found;
}

static void remove_client_context(ClientContext* ctx)
{
    if (!ctx)
    {
        return;
    }

    /* Prevent double-removal */
    if (InterlockedExchange(&ctx->closed, 1) != 0)
    {
        return;
    }

    shutdown(ctx->sock, SD_BOTH);
    closesocket(ctx->sock);

    EnterCriticalSection(&g_client_lock);

    for (int i = 0; i < g_client_count; i++)
    {
        if (g_clients[i] == ctx)
        {
            g_clients[i] = g_clients[g_client_count - 1];
            g_clients[g_client_count - 1] = NULL;
            g_client_count--;
            break;
        }
    }

    LeaveCriticalSection(&g_client_lock);

    free(ctx);
}

static void remove_client_by_socket(SOCKET sock)
{
    ClientContext* ctx = find_client_by_socket(sock);
    if (ctx)
    {
        remove_client_context(ctx);
    }
}

/* -------------------- sending / broadcast -------------------- */

static int send_all_socket(SOCKET s, const char* buf, int len, int* wsa_error)
{
    int total = 0;

    while (total < len)
    {
        int n = send(s, buf + total, len - total, 0);

        if (n == SOCKET_ERROR)
        {
            if (wsa_error)
            {
                *wsa_error = WSAGetLastError();
            }
            return SOCKET_ERROR;
        }

        if (n == 0)
        {
            if (wsa_error)
            {
                *wsa_error = WSAECONNRESET;
            }
            return SOCKET_ERROR;
        }

        total += n;
    }

    return 0;
}

static void broadcast_message(const char* message, size_t message_len, SOCKET sender_sock)
{
    SOCKET sockets[MAX_CLIENTS];
    int count = 0;

    EnterCriticalSection(&g_client_lock);
    for (int i = 0; i < g_client_count; i++)
    {
        if (g_clients[i])
        {
            sockets[count++] = g_clients[i]->sock;
        }
    }
    LeaveCriticalSection(&g_client_lock);

    if (message_len > CLIENT_BUFFER_SIZE)
    {
        message_len = CLIENT_BUFFER_SIZE;
    }

    /* Keep newline framing for the chat clients */
    char outbuf[CLIENT_BUFFER_SIZE + 2];
    memcpy(outbuf, message, message_len);
    outbuf[message_len] = '\n';
    int out_len = (int)message_len + 1;

    for (int i = 0; i < count; i++)
    {
        SOCKET target = sockets[i];

        if (target == sender_sock)
        {
            continue;
        }

        int err = 0;
        if (send_all_socket(target, outbuf, out_len, &err) == SOCKET_ERROR)
        {
            /* Remove dead sockets so they do not keep poisoning broadcasts */
            if (err == WSAECONNRESET ||
                err == WSAENOTCONN ||
                err == WSAESHUTDOWN ||
                err == WSAECONNABORTED)
            {
                remove_client_by_socket(target);
            }
        }
    }
}

/* -------------------- parsing / benchmark processing -------------------- */

static void process_received_data(ClientContext* ctx, const char* data, size_t len)
{
    if (ctx->msg_len + len >= CLIENT_BUFFER_SIZE)
    {
        /* Malformed or oversized stream; drop the client */
        remove_client_context(ctx);
        return;
    }

    memcpy(ctx->msg_buf + ctx->msg_len, data, len);
    ctx->msg_len += len;

    while (1)
    {
        char* newline = (char*)memchr(ctx->msg_buf, '\n', ctx->msg_len);
        if (!newline)
        {
            break;
        }

        size_t message_len = (size_t)(newline - ctx->msg_buf);

        char message[CLIENT_BUFFER_SIZE];
        if (message_len >= sizeof(message))
        {
            message_len = sizeof(message) - 1;
        }

        memcpy(message, ctx->msg_buf, message_len);
        message[message_len] = '\0';

        int client_id = -1;
        int sequence = -1;
        double send_time = 0.0;

        if (sscanf(message, "%d,%d,%lf", &client_id, &sequence, &send_time) == 3)
        {
            double receive_time = now_seconds();
            double latency_ms = (receive_time - send_time) * 1000.0;

            printf("\n===================================\n");
            printf("Message received\n");
            printf("Socket FD : %llu\n", (unsigned long long)ctx->sock);
            printf("Client ID : %d\n", client_id);
            printf("Sequence  : %d\n", sequence);
            printf("Size      : %zu bytes\n", message_len);
            printf("Latency   : %.3f ms\n", latency_ms);
            printf("===================================\n");

            write_record(ctx->sock, receive_time, client_id, sequence, send_time, latency_ms);
        }

        /* Broadcast the message to other connected clients */
        broadcast_message(message, message_len, ctx->sock);

        size_t consumed = message_len + 1;
        memmove(ctx->msg_buf, ctx->msg_buf + consumed, ctx->msg_len - consumed);
        ctx->msg_len -= consumed;
    }
}

/* -------------------- IOCP recv posting -------------------- */

static void post_recv(ClientContext* ctx)
{
    if (InterlockedCompareExchange(&ctx->closed, 0, 0) != 0)
    {
        return;
    }

    ZeroMemory(&ctx->recv_ov, sizeof(ctx->recv_ov));
    ctx->recv_wsabuf.buf = ctx->recv_buf;
    ctx->recv_wsabuf.len = RECV_CHUNK;

    DWORD flags = 0;
    DWORD bytes = 0;

    int rc = WSARecv(ctx->sock,
                     &ctx->recv_wsabuf,
                     1,
                     &bytes,
                     &flags,
                     &ctx->recv_ov,
                     NULL);

    if (rc == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            remove_client_context(ctx);
        }
    }
}

/* -------------------- worker thread -------------------- */

static DWORD WINAPI worker_thread(LPVOID param)
{
    (void)param;

    while (1)
    {
        DWORD bytes_transferred = 0;
        ULONG_PTR completion_key = 0;
        LPOVERLAPPED pov = NULL;

        BOOL ok = GetQueuedCompletionStatus(
            g_iocp,
            &bytes_transferred,
            &completion_key,
            &pov,
            INFINITE);

        (void)completion_key;

        if (pov == NULL)
        {
            /* Shutdown packet */
            if (!g_running)
            {
                break;
            }
            continue;
        }

        ClientContext* ctx = CONTAINING_RECORD(pov, ClientContext, recv_ov);

        if (!ok)
        {
            remove_client_context(ctx);
            continue;
        }

        if (bytes_transferred == 0)
        {
            remove_client_context(ctx);
            continue;
        }

        process_received_data(ctx, ctx->recv_buf, bytes_transferred);

        if (InterlockedCompareExchange(&ctx->closed, 0, 0) == 0)
        {
            post_recv(ctx);
        }
    }

    return 0;
}

/* -------------------- console shutdown handler -------------------- */

static BOOL WINAPI console_handler(DWORD type)
{
    if (type == CTRL_C_EVENT ||
        type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT)
    {
        InterlockedExchange(&g_running, 0);

        if (g_listen_sock != INVALID_SOCKET)
        {
            closesocket(g_listen_sock);
            g_listen_sock = INVALID_SOCKET;
        }

        /* Wake workers so they can exit */
        for (int i = 0; i < g_worker_count; i++)
        {
            PostQueuedCompletionStatus(g_iocp, 0, 0, NULL);
        }

        return TRUE;
    }

    return FALSE;
}

/* -------------------- main -------------------- */

int main(int argc, char* argv[])
{
    const char* csv_path = "../results/raw/benchmark_400_clients_iocp.csv";
    int port = SERVER_PORT;

    if (argc > 1)
    {
        csv_path = argv[1];
    }

    if (argc > 2)
    {
        port = atoi(argv[2]);
    }

    if (QueryPerformanceFrequency(&g_qpc_freq) == 0)
    {
        fprintf(stderr, "QueryPerformanceFrequency failed\n");
        return 1;
    }

    InitializeCriticalSection(&g_client_lock);
    InitializeCriticalSection(&g_log_lock);
    SetConsoleCtrlHandler(console_handler, TRUE);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    open_log(csv_path);

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_iocp == NULL)
    {
        fprintf(stderr, "CreateIoCompletionPort failed\n");
        WSACleanup();
        return 1;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);

    g_worker_count = (int)si.dwNumberOfProcessors * 2;
    if (g_worker_count < 2)
    {
        g_worker_count = 2;
    }
    if (g_worker_count > 16)
    {
        g_worker_count = 16;
    }

    g_worker_handles = (HANDLE*)calloc((size_t)g_worker_count, sizeof(HANDLE));
    if (!g_worker_handles)
    {
        fprintf(stderr, "calloc failed\n");
        WSACleanup();
        return 1;
    }

    for (int i = 0; i < g_worker_count; i++)
    {
        g_worker_handles[i] = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (g_worker_handles[i] == NULL)
        {
            fprintf(stderr, "CreateThread failed\n");
            WSACleanup();
            return 1;
        }
    }

    /* Create listening socket */
    g_listen_sock = WSASocket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED);

    if (g_listen_sock == INVALID_SOCKET)
    {
        fprintf(stderr, "WSASocket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    ZeroMemory(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((u_short)port);

    if (bind(g_listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
    {
        fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
        closesocket(g_listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(g_listen_sock, SOMAXCONN) == SOCKET_ERROR)
    {
        fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
        closesocket(g_listen_sock);
        WSACleanup();
        return 1;
    }

    printf("IOCP TCP chat server running on port %d\n", port);
    printf("CSV: %s\n", csv_path);
    printf("Worker threads: %d\n", g_worker_count);

    printf("ENTERING ACCEPT LOOP\n");
    fflush(stdout);

    while (InterlockedCompareExchange(&g_running, 0, 0) != 0)
    {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);

        SOCKET client_sock = accept(g_listen_sock, (struct sockaddr*)&client_addr, &addr_len);
        printf("ACCEPT RETURNED\n");
        fflush(stdout);
        if (client_sock == INVALID_SOCKET)
        {
            int err = WSAGetLastError();
            if (!InterlockedCompareExchange(&g_running, 0, 0))
            {
                break;
            }

            /* Accept can fail briefly during shutdown or transient errors */
            if (err == WSAEINTR)
            {
                break;
            }

            Sleep(1);
            continue;
        }

        ClientContext* ctx = (ClientContext*)calloc(1, sizeof(ClientContext));
        if (!ctx)
        {
            closesocket(client_sock);
            continue;
        }

        ctx->sock = client_sock;
        ctx->msg_len = 0;
        ctx->closed = 0;

        /* Associate this socket with the IO completion port */
        if (CreateIoCompletionPort((HANDLE)client_sock, g_iocp, 0, 0) == NULL)
        {
            closesocket(client_sock);
            free(ctx);
            continue;
        }

        add_client(ctx);
        post_recv(ctx);

        printf("Client connected: %s\n", inet_ntoa(client_addr.sin_addr));
    }

    /* Trigger worker shutdown */
    InterlockedExchange(&g_running, 0);
    for (int i = 0; i < g_worker_count; i++)
    {
        PostQueuedCompletionStatus(g_iocp, 0, 0, NULL);
    }
    printf("Waiting for threads...\n");
    WaitForMultipleObjects((DWORD)g_worker_count, g_worker_handles, TRUE, INFINITE);
    printf("Threads finished\n");
    for (int i = 0; i < g_worker_count; i++)
    {
        CloseHandle(g_worker_handles[i]);
    }

    free(g_worker_handles);

    /* Cleanup remaining clients */
    EnterCriticalSection(&g_client_lock);
    for (int i = 0; i < g_client_count; i++)
    {
        if (g_clients[i] && InterlockedCompareExchange(&g_clients[i]->closed, 1, 1) == 0)
        {
            shutdown(g_clients[i]->sock, SD_BOTH);
            closesocket(g_clients[i]->sock);
            free(g_clients[i]);
            g_clients[i] = NULL;
        }
    }
    g_client_count = 0;
    LeaveCriticalSection(&g_client_lock);

    if (g_listen_sock != INVALID_SOCKET)
    {
        closesocket(g_listen_sock);
    }

    if (g_log_file)
    {
        fclose(g_log_file);
    }

    if (g_iocp)
    {
        CloseHandle(g_iocp);
    }

    DeleteCriticalSection(&g_client_lock);
    DeleteCriticalSection(&g_log_lock);

    WSACleanup();
    return 0;
}