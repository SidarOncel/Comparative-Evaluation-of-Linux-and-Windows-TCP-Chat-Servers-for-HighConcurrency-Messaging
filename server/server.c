#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // General utilities
#include <string.h>     // memset()
#include <unistd.h>     // close()

// Networking headers
#include <arpa/inet.h>  // inet_ntoa(), htons()
#include <sys/socket.h> // socket(), bind(), listen(), accept()
#include <netinet/in.h> // sockaddr_in

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
    server_addr.sin_port = htons(8080);

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

    printf("Listening on port 8080...\n");
    printf("Waiting for incoming connection...\n");

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

    /*
     * Close client connection
     */
    close(client_fd);

    /*
     * Close listening socket
     */
    close(server_fd);

    printf("Server shutdown.\n");

    return 0;
}