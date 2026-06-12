#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // General utilities
#include <string.h>     // memset()
#include <unistd.h>     // close()

// Networking headers
#include <arpa/inet.h>  // inet_ntoa(), htons()
#include <sys/socket.h> // socket(), bind(), listen(), accept()
#include <netinet/in.h> // sockaddr_in
#include <pthread.h>


#define MAX_CLIENTS 5000

/*
 * Stores all active client sockets.
 */
int clients[MAX_CLIENTS];

/*
 * Number of currently connected clients.
 */
int client_count = 0;

pthread_mutex_t clients_mutex =
    PTHREAD_MUTEX_INITIALIZER;

/*
 * Thread function.
 *
 * Every connected client gets its own thread.
 */
void broadcast_message(
    char* message,
    int sender_fd){
    pthread_mutex_lock(
        &clients_mutex
    );

    for(int i = 0;
        i < client_count;
        i++)
    {
        if(clients[i] != sender_fd)
        {
            send(
                clients[i],
                message,
                strlen(message),
                0
            );
        }
    }

    pthread_mutex_unlock(
        &clients_mutex
    );
}


void* handle_client(void* arg)
{
    /*
     * Retrieve client socket from argument.
     */
    int client_fd = *((int*)arg);
    free(arg);

    char buffer[1024];

    while (1)
    {
        memset(buffer, 0, sizeof(buffer));

        int bytes_received =
            recv(client_fd,
                 buffer,
                 sizeof(buffer) - 1,
                 0);

        if (bytes_received == 0)
        {
            printf("\nClient disconnected.\n");
            break;
        }

        if (bytes_received < 0)
        {
            perror("recv");
            break;
        }

        buffer[bytes_received] = '\0';

        printf("\nMessage received:\n");
        printf("%s\n", buffer);

        broadcast_message(
            buffer,
            client_fd
        );

        printf("Response sent.\n");
    }
    pthread_mutex_lock(
        &clients_mutex
    );

    for(int i = 0;
        i < client_count;
        i++)
    {
        if(clients[i] ==
        client_fd)
        {
            clients[i] =
                clients[client_count - 1];

            client_count--;

            break;
        }
    }
    pthread_mutex_unlock(
        &clients_mutex
    );

    close(client_fd);

    return NULL;
}

int main()
{
    
    // Server socket file descriptor
    // Think of this as a handle to our listening endpoint
    int server_fd;

    /*
     * Create a TCP socket
     *
     * AF_INET     -> IPv4
     * SOCK_STREAM -> TCP
     * 0           -> Use default protocol (TCP)
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;

    if (setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opt,
            sizeof(opt)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }
    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    printf("Socket created successfully.\n");

    /*
     * sockaddr_in stores the server address information
     */
    struct sockaddr_in server_addr;

    // Initialize structure to zero
    memset(&server_addr, 0, sizeof(server_addr));

    /*
     * Configure address
     */
    server_addr.sin_family = AF_INET;

    /*
     * Port number
     *
     * htons() converts host byte order
     * to network byte order (Big Endian)
     */
    server_addr.sin_port = htons(9090);

    /*
     * Accept connections on all interfaces
     *
     * Examples:
     * 127.0.0.1
     * 192.168.x.x
     */
    server_addr.sin_addr.s_addr = INADDR_ANY;

    /*
     * Bind socket to address and port
     * After this step:
     * Server -> Port 8080
     */
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return 1;
    }

    printf("Bind successful.\n");

    /*
     * Put socket into listening mode
     *
     * Backlog = 10
     *
     * The kernel can queue up to
     * 10 pending connection requests.
     */
    if (listen(server_fd, 10) < 0)
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Listening on port 9090...\n");
    printf("Waiting for incoming connection...\n");

    while (1)
    {
        /*
        * Information about the connecting client
        */
        struct sockaddr_in client_addr;

        socklen_t client_len = sizeof(client_addr);

        /*
        * accept() blocks until a client connects
        *
        * Important:
        *
        * server_fd remains the listening socket
        *
        * accept() creates a NEW socket
        * dedicated to the connected client.
        */
        int client_fd =
            accept(server_fd,
                (struct sockaddr *)&client_addr,
                &client_len);

        if (client_fd < 0)
        {
            perror("accept");
            close(server_fd);
            return 1;
        }

        /*
        * Print client IP address
        */
        printf("\nClient connected!\n");
        printf("Client IP: %s\n",
            inet_ntoa(client_addr.sin_addr));
        pthread_mutex_lock(
            &clients_mutex
        );

        clients[client_count] =
            client_fd;

        client_count++;

        pthread_mutex_unlock(
            &clients_mutex
        );

        printf(
            "Total clients: %d\n",
            client_count
        );
            
        pthread_t tid;

        int* client_socket =
            malloc(sizeof(int));

        *client_socket = client_fd;

        pthread_create(
            &tid,
            NULL,
            handle_client,
            client_socket
        );
    
        pthread_detach(tid);
    }
    
    /*
     * Close listening socket
     */
    close(server_fd);

    printf("Server shutdown.\n");

    return 0;
}