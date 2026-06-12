/*
 * ============================================================
 * epoll_server_v2.c
 * ============================================================
 *
 * High-Concurrency TCP Chat Server
 *
 * Features:
 *  - Linux epoll I/O multiplexing
 *  - Non-blocking sockets
 *  - Multiple concurrent clients
 *  - Broadcast messaging
 *  - Benchmark CSV logging
 *  - TCP stream reassembly
 *  - Message framing using '\n'
 *  - Large payload support
 *
 * Project:
 * Comparative Evaluation of Linux and Windows
 * TCP Chat Servers for High-Concurrency Messaging
 *
 * ============================================================
 */

#include <stdio.h>          // printf(), fprintf(), perror()
#include <stdlib.h>         // exit(), EXIT_FAILURE
#include <string.h>         // memset(), memcpy(), memmove(), memchr()
#include <unistd.h>         // close()
#include <errno.h>          // errno
#include <fcntl.h>          // fcntl()

#include <arpa/inet.h>      // inet_ntoa(), htons()
#include <netinet/in.h>     // sockaddr_in
#include <sys/socket.h>     // socket(), bind(), listen(), accept()
#include <sys/epoll.h>      // epoll API

#include <time.h>           // clock_gettime()

/*
 * ============================================================
 * Configuration Constants
 * ============================================================
 */

/*
 * Server TCP port.
 *
 * Clients will connect to:
 *
 * 127.0.0.1:9090
 */
#define SERVER_PORT 9090

/*
 * Maximum events returned by epoll_wait()
 * in a single call.
 */
#define MAX_EVENTS 1024

/*
 * Maximum clients tracked by the server.
 *
 * This is NOT a hard operating-system limit.
 * It is simply the size of our array.
 */
#define MAX_CLIENTS 5000

/*
 * Temporary receive chunk size.
 *
 * recv() reads data into this buffer.
 *
 * TCP may split a message into many chunks,
 * therefore this is NOT the message size.
 */
#define RECV_BUFFER_SIZE 4096

/*
 * Per-client stream buffer.
 *
 * Stores accumulated TCP data until
 * complete messages are found.
 *
 * Supports large payload benchmarks.
 */
#define CLIENT_BUFFER_SIZE 1536

/*
 * ============================================================
 * Client Structure
 * ============================================================
 *
 * TCP is a stream protocol.
 *
 * One recv() does NOT equal one message.
 *
 * Therefore every client needs its own
 * accumulation buffer.
 */
typedef struct
{
    /*
     * Client socket file descriptor.
     */
    int fd;

    /*
     * Stores accumulated TCP stream data.
     */
    char buffer[CLIENT_BUFFER_SIZE];

    /*
     * Number of bytes currently stored
     * in the buffer.
     */
    size_t buffer_length;

} Client;

/*
 * ============================================================
 * Global Variables
 * ============================================================
 */

/*
 * All connected clients.
 */
Client clients[MAX_CLIENTS];

/*
 * Number of currently connected clients.
 */
int client_count = 0;

/*
 * Benchmark CSV log file.
 */
FILE* log_file = NULL;

/*
 * ============================================================
 * Helper Functions
 * ============================================================
 */

/*
 * Find a client structure using
 * a socket file descriptor.
 *
 * Returns:
 *   Pointer to Client if found
 *   NULL otherwise
 */
Client* find_client(int fd)
{
    for(int i = 0; i < client_count; i++)
    {
        if(clients[i].fd == fd)
        {
            return &clients[i];
        }
    }

    return NULL;
}
/*
 * ============================================================
 * Make Socket Non-Blocking
 * ============================================================
 *
 * epoll works best with non-blocking sockets.
 *
 * Without O_NONBLOCK:
 *
 * recv()
 * accept()
 *
 * can stop the entire server.
 */
int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if(flags == -1)
    {
        perror("fcntl(F_GETFL)");
        return -1;
    }

    if(fcntl(fd,
             F_SETFL,
             flags | O_NONBLOCK) == -1)
    {
        perror("fcntl(F_SETFL)");
        return -1;
    }

    return 0;
}

/*
 * ============================================================
 * Add Client
 * ============================================================
 *
 * Called immediately after accept().
 *
 * Creates an entry inside our client table.
 */
void add_client(int fd)
{
    if(client_count >= MAX_CLIENTS)
    {
        printf("Maximum clients reached.\n");

        close(fd);

        return;
    }

    clients[client_count].fd = fd;

    clients[client_count].buffer_length = 0;

    memset(
        clients[client_count].buffer,
        0,
        sizeof(clients[client_count].buffer)
    );

    client_count++;

    printf("-----------------------------------\n");
    printf("Client added\n");
    printf("Socket FD: %d\n", fd);
    printf("Total Clients: %d\n", client_count);
    printf("-----------------------------------\n");
}

/*
 * ============================================================
 * Remove Client
 * ============================================================
 *
 * Called when:
 *
 * recv() returns 0
 *
 * OR
 *
 * recv() fails
 *
 * OR
 *
 * client disconnects unexpectedly
 */
void remove_client(int fd)
{
    for(int i = 0; i < client_count; i++)
    {
        if(clients[i].fd == fd)
        {
            /*
             * Replace removed client
             * with last client.
             *
             * O(1) removal.
             */
            clients[i] =
                clients[client_count - 1];

            client_count--;

            break;
        }
    }

    close(fd);

    printf("-----------------------------------\n");
    printf("Client removed\n");
    printf("Socket FD: %d\n", fd);
    printf("Remaining Clients: %d\n", client_count);
    printf("-----------------------------------\n");
}

/*
 * ============================================================
 * Broadcast Message
 * ============================================================
 *
 * Send a message to all connected clients
 * except the sender.
 *
 * Chat-style behaviour.
 */
void broadcast_message(
    const char* message,
    int sender_fd)
{
    size_t message_length =
        strlen(message);

    for(int i = 0; i < client_count; i++)
    {
        int target_fd =
            clients[i].fd;

        /*
         * Do not send back to sender.
         */
        if(target_fd == sender_fd)
        {
            continue;
        }

        ssize_t bytes_sent =
            send(
                target_fd,
                message,
                message_length,
                MSG_DONTWAIT
            );

        if(bytes_sent < 0)
        {
        if(errno == EPIPE ||
            errno == ECONNRESET)
            {
                close(target_fd);
                remove_client(target_fd);
                i--;
            }
        }
    }
}

/*
 * ============================================================
 * Open Benchmark CSV
 * ============================================================
 *
 * Creates:
 *
 * results/raw/.csv
 *
 * Stores:
 *
 * socket_fd,
 * timestamp,
 * message
 */
int open_benchmark_log()
{
    log_file =
        fopen(
            "../results/raw/benchmark_100_clients.csv",
            "a"
        );

    if(log_file == NULL)
    {
        perror("fopen");
        return -1;
    }

    /*
     * CSV Header
     */
    fprintf(
        log_file,
        "socket_fd,receive_time,client_id,sequence,send_time,latency_ms\n"
    );

    fflush(log_file);

    printf("-----------------------------------\n");
    printf("Benchmark log opened.\n");
    printf("-----------------------------------\n");

    return 0;
}

/*
 * ============================================================
 * Write Benchmark Record
 * ============================================================
 *
 * Every complete message generates
 * one CSV entry.
 */
void log_message(int fd, const char* message)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    double receive_time =
        (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

    fprintf(
        log_file,
        "%d,%.6f,%s",
        fd,
        receive_time,
        message
    );

    if (message[strlen(message) - 1] != '\n')
    {
        fputc('\n', log_file);
    }

    fflush(log_file);
}

/*
 * ============================================================
 * Extract Complete Messages
 * ============================================================
 *
 * TCP does NOT preserve message boundaries.
 *
 * Example:
 *
 * recv():
 *
 * msg1\nmsg2\nmsg3\n
 *
 * We must split manually.
 *
 * This function processes every complete
 * newline-terminated message currently
 * inside a client's stream buffer.
 */
void process_client_buffer(
    Client* client)
{
    char* newline;

    while(
        (newline =
            memchr(
                client->buffer,
                '\n',
                client->buffer_length
            )) != NULL
    )
    {
        /*
         * Length of complete message.
         */
        size_t message_length =
            newline -
            client->buffer;

        char message[CLIENT_BUFFER_SIZE];

        memcpy(
            message,
            client->buffer,
            message_length
        );
        message[message_length] = '\0';

        /*
        * Benchmark message format:
        *
        * client_id,
        * sequence_number,
        * send_timestamp
        *
        * Example:
        *
        * 12,4,1781171994.695060
        */
        int client_id;
        int sequence;
        double send_time;

        /*
        * Parse benchmark message.
        */
        if(
            sscanf(
                message,
                "%d,%d,%lf",
                &client_id,
                &sequence,
                &send_time
            ) == 3
        )
        {
            struct timespec ts;

            clock_gettime(
                CLOCK_MONOTONIC,
                &ts
            );

            double receive_time =
                (double)ts.tv_sec +
                (double)ts.tv_nsec / 1e9;

            double latency_ms =
                (receive_time - send_time) *
                1000.0;

            printf("\n");
            printf("===================================\n");
            printf("Message received\n");
            printf("Socket FD : %d\n", client->fd);
            printf("Client ID : %d\n", client_id);
            printf("Sequence  : %d\n", sequence);
            printf("Size      : %zu bytes\n",
                message_length);
            printf("Latency   : %.3f ms\n",
                latency_ms);
            printf("===================================\n");

            /*
            * Save benchmark record.
            */
            fprintf(
                log_file,
                "%d,%.6f,%d,%d,%.6f,%.3f\n",
                client->fd,
                receive_time,
                client_id,
                sequence,
                send_time,
                latency_ms
            );

            fflush(log_file);
        }
        else
        {
            /*
            * Fallback for normal chat messages.
            */
            log_message(
                client->fd,
                message
            );
        }

        /*
        * Forward message.
        */
        broadcast_message(
            message,
            client->fd
        );
                /*
         * Remove processed message
         * from stream buffer.
         *
         * +1 removes '\n'
         */
        size_t consumed =
            message_length + 1;

        memmove(
            client->buffer,
            client->buffer + consumed,
            client->buffer_length - consumed
        );

        client->buffer_length -=
            consumed;
    }
}

/*
 * ============================================================
 * Main Function
 * ============================================================
 */
int main()
{
    /*
     * Server listening socket.
     */
    int server_fd;

    /*
     * epoll instance.
     */
    int epoll_fd;

    /*
     * Create TCP socket.
     *
     * AF_INET     -> IPv4
     * SOCK_STREAM -> TCP
     */
    server_fd =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if(server_fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    printf("Socket created.\n");

    /*
     * Allow immediate port reuse.
     *
     * Prevents:
     *
     * bind:
     * Address already in use
     */
    int opt = 1;

    if(
        setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opt,
            sizeof(opt)
        ) < 0
    )
    {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * Make listening socket non-blocking.
     */
    if(set_nonblocking(server_fd) < 0)
    {
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * Server address configuration.
     */
    struct sockaddr_in server_addr;

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(SERVER_PORT);

    server_addr.sin_addr.s_addr =
        INADDR_ANY;

    /*
     * Bind socket.
     */
    if(
        bind(
            server_fd,
            (struct sockaddr*)&server_addr,
            sizeof(server_addr)
        ) < 0
    )
    {
        perror("bind");

        close(server_fd);

        return EXIT_FAILURE;
    }

    printf(
        "Bind successful on port %d\n",
        SERVER_PORT
    );

    /*
     * Listen for incoming clients.
     */
    if(
        listen(
            server_fd,
            SOMAXCONN
        ) < 0
    )
    {
        perror("listen");

        close(server_fd);

        return EXIT_FAILURE;
    }

    printf("Server listening...\n");

    /*
     * Open benchmark CSV.
     */
    if(open_benchmark_log() < 0)
    {
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * Create epoll instance.
     */
    epoll_fd =
        epoll_create1(0);

    if(epoll_fd < 0)
    {
        perror("epoll_create1");

        fclose(log_file);

        close(server_fd);

        return EXIT_FAILURE;
    }

    printf("epoll instance created.\n");

    /*
     * Event structure for
     * listening socket.
     */
    struct epoll_event event;

    event.events =
        EPOLLIN;

    event.data.fd =
        server_fd;

    /*
     * Register listening socket
     * with epoll.
     */
    if(
        epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            server_fd,
            &event
        ) < 0
    )
    {
        perror("epoll_ctl");

        fclose(log_file);

        close(server_fd);

        close(epoll_fd);

        return EXIT_FAILURE;
    }

    printf(
        "Listening socket added to epoll.\n"
    );

    /*
     * Event array.
     *
     * epoll_wait()
     * fills this array.
     */
    struct epoll_event
        events[MAX_EVENTS];

    printf("\n");
    printf(
        "====================================\n"
    );
    printf(
        "epoll_server_v2 running...\n"
    );
    printf(
        "====================================\n"
    );

    /*
     * Main event loop.
     */
    while(1)
    {
        int ready_fds =
            epoll_wait(
                epoll_fd,
                events,
                MAX_EVENTS,
                -1
            );

        if(ready_fds < 0)
        {
            perror("epoll_wait");
            break;
        }

        /*
         * Process all ready events.
         */
        for(
            int i = 0;
            i < ready_fds;
            i++
        )
        {
            int fd =
                events[i].data.fd;

            /*
             * New incoming connection.
             */
            if(fd == server_fd)
            {/*
        * Accept ALL pending clients.
        *
        * Since the listening socket is
        * non-blocking, accept() may have
        * multiple pending connections.
        */
            while(1)
            {
                struct sockaddr_in client_addr;

                socklen_t client_len =
                    sizeof(client_addr);

                int client_fd =
                    accept(
                        server_fd,
                        (struct sockaddr*)&client_addr,
                        &client_len
                    );

                /*
                * No more pending clients.
                */
                if(client_fd < 0)
                {
                    if(errno == EAGAIN ||
                    errno == EWOULDBLOCK)
                    {
                        break;
                    }

                    perror("accept");
                    break;
                }

                /*
                * Make client non-blocking.
                */
                if(set_nonblocking(client_fd) < 0)
                {
                    close(client_fd);
                    continue;
                }

                printf("\n");
                printf(
                    "Client connected\n"
                );

                printf(
                    "IP: %s\n",
                    inet_ntoa(
                        client_addr.sin_addr
                    )
                );

                /*
                * Register client with epoll.
                */
                struct epoll_event client_event;

                client_event.events =
                    EPOLLIN;

                client_event.data.fd =
                    client_fd;

                if(
                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_ADD,
                        client_fd,
                        &client_event
                    ) < 0
                )
                {
                    perror("epoll_ctl");

                    close(client_fd);

                    continue;
                }

                /*
                * Add to client table.
                */
            add_client(client_fd);

            }   /* closes while(1) */

            }   /* closes if(fd == server_fd) */

            else
            {
                char recv_buffer[RECV_BUFFER_SIZE];

                int bytes_received =
                    recv(
                        fd,
                        recv_buffer,
                        sizeof(recv_buffer),
                        0
                    );

                /*
                * Client disconnected.
                */
                if(bytes_received == 0)
                {
                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_DEL,
                        fd,
                        NULL
                    );

                    remove_client(fd);

                    continue;
                }

                /*
                * Receive error.
                */
                if(bytes_received < 0)
                {
                    if(errno != EAGAIN &&
                    errno != EWOULDBLOCK)
                    {
                        perror("recv");

                        epoll_ctl(
                            epoll_fd,
                            EPOLL_CTL_DEL,
                            fd,
                            NULL
                        );

                        remove_client(fd);
                    }

                    continue;
                }

                Client* client =
                    find_client(fd);

                if(client == NULL)
                {
                    continue;
                }
            /*
            * Prevent buffer overflow.
            */
            if(
                client->buffer_length +
                bytes_received
                >=
                CLIENT_BUFFER_SIZE
            )
            {
                printf(
                    "Client buffer full.\n"
                );

                epoll_ctl(
                    epoll_fd,
                    EPOLL_CTL_DEL,
                    fd,
                    NULL
                );

                remove_client(fd);

                continue;
            }

                memcpy(
                    client->buffer +
                    client->buffer_length,

                    recv_buffer,

                    bytes_received
                );

                client->buffer_length +=
                    bytes_received;
                    process_client_buffer(
                    client
                );
            }
        }
    }
    /*
    * Cleanup
    */
    printf(
        "\nServer shutting down...\n"
    );

    if(log_file)
    {
        fclose(log_file);
    }

    close(server_fd);

    close(epoll_fd);

    return EXIT_SUCCESS;
}