#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/md5.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * help() - Print a help message
 */
void help(char *progname) {
    printf("Usage: %s [OPTIONS]\n", progname);
    printf("Initiate a network file server\n");
    printf("  -l    number of entries in cache\n");
    printf("  -p    port on which to listen for connections\n");
}

/*
 * die() - print an error and exit the program
 */
void die(const char *msg1, char *msg2) {
    fprintf(stderr, "%s, %s\n", msg1, msg2);
    exit(0);
}

/*
 * open_server_socket() - Open a listening socket and return its file
 *                        descriptor, or terminate the program
 */
int open_server_socket(int port) {
    int                listenfd;    /* the server's listening file descriptor */
    struct sockaddr_in addrs;       /* describes which clients we'll accept */
    int                optval = 1;  /* for configuring the socket */

    /* Create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("Error creating socket: ", strerror(errno));

    /* Eliminates "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optval , sizeof(int)) < 0)
        die("Error configuring socket: ", strerror(errno));

    /* Listenfd will be an endpoint for all requests to the port from any IP
       address */
    bzero((char *) &addrs, sizeof(addrs));
    addrs.sin_family = AF_INET;
    addrs.sin_addr.s_addr = htonl(INADDR_ANY);
    addrs.sin_port = htons((unsigned short)port);
    if (bind(listenfd, (struct sockaddr *)&addrs, sizeof(addrs)) < 0)
        die("Error in bind(): ", strerror(errno));

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, 1024) < 0)  // backlog of 1024
        die("Error in listen(): ", strerror(errno));

    return listenfd;
}

/*
 * handle_requests() - given a listening file descriptor, continually wait
 *                     for a request to come in, and when it arrives, pass it
 *                     to service_function.  Note that this is not a
 *                     multi-threaded server.
 */
void handle_requests(int listenfd, void (*service_function)(int, int), int param) {
    while (1) {
        /* block until we get a connection */
        struct sockaddr_in clientaddr;
        int clientlen = sizeof(clientaddr);
        int connfd;
        if ((connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen)) < 0)
            die("Error in accept(): ", strerror(errno));

        /* print some info about the connection */
        struct hostent *hp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr,
                           sizeof(clientaddr.sin_addr.s_addr), AF_INET);
        if (hp == NULL) {
            fprintf(stderr, "DNS error in gethostbyaddr() %d\n", h_errno);
            exit(0);
        }
        char *haddrp = inet_ntoa(clientaddr.sin_addr);
        printf("server connected to %s (%s)\n", hp->h_name, haddrp);

        /* serve requests */
        service_function(connfd, param);

        /* clean up, await new connection */
        if (close(connfd) < 0)
            die("Error in close(): ", strerror(errno));
    }
}

/*
 * - Receive() - recv wrapper
 */
unsigned char Receive(int connfd, void * buffer, int length){
    unsigned char * buf_location = (unsigned char * )buffer;
    unsigned char return_value = 0;
    int bytes_received = 1;
    while(length && !return_value){
        bytes_received = recv(connfd, buffer, length, 0);
        if(!bytes_received){
            return_value = 1;
        }
        else if(bytes_received == -1){
            if(errno != EINTR)
                die("read error: ", strerror(errno));
            continue;
        }
        else{
            length -= bytes_received;
            buf_location += bytes_received;
        }
    }
    return return_value;
}
/*
 * - Receive_Int() - uses Receive() to grab one int
 */
uint32_t Receive_Int(int connfd){
    uint32_t return_value;
    Receive(connfd, &return_value, sizeof(return_value));
    return return_value;
    //return ntohl(return_value);
}
/*
 * - Send() - write wrapper
 */
void Send(int connfd, void * buffer, int length)
{
    unsigned char * buf_location = (unsigned char * )buffer;
    int bytes_sent = 1;
    while(length){
        if ((bytes_sent = write(connfd, buf_location, length)) < 0)
        {
            if (errno != EINTR)
                die("Put_file write error", strerror(errno));
            bytes_sent = 0;
        }
        length -= bytes_sent;
        buf_location += bytes_sent;
    }
}
/*
 * - Send_Int() - uses Send() to send one int
 */
void Send_Int(int connfd, uint32_t val){
    Send(connfd, &val, sizeof(val));
    //return return_value;
    //return ntohl(return_value);
}

/*
 * send_error() - send an error back to the client
 */
void send_error(int connfd, char * msg)
{
    fprintf(stderr, "Sending error message to client: %s", msg);
    Send_Int(connfd, strlen(msg));
    Send(connfd, msg, strlen(msg));
}

/*
 * - file_server() - etc
 */
void file_server(int connfd, int lru_size){
    /* static vars for managing LRU cache */

    /* read the header size from the client */
    uint32_t headersize;
    headersize = Receive_Int(connfd);
    /* read the header from the client */
    char header[headersize];
    if(Receive(connfd, header, headersize) != 0){
        die("SERVER FILE_SERVER", "Connection closed while reading header");
    }
    /* parse the header */
    char * request_type = strtok(header, "\n");
    int is_put = (strcmp(request_type, "PUT") == 0);
    int is_get = (strcmp(request_type, "GET") == 0);
    if(!is_put && !is_get){
        send_error(connfd, "Request must begin with PUT or GET\n");
        return;
    }
    char * filename;
    if((filename = strtok(NULL, "\n")) == NULL){
        send_error(connfd, "Request must include filename\n");
        return;
    }
    /* handle PUT */
    if(is_put){
        char * filesize_str;
        if((filesize_str = strtok(NULL, "\n")) == NULL){
            send_error(connfd, "PUT request must include filesize\n");
            return;
        }
        char * t;
        long filesize = strtol(filesize_str, &t, 10);
        if(filesize_str == t){
            send_error(connfd, "Invalid filesize in PUT request\n");
            return;
        }
        char file[filesize];
        if(Receive(connfd, file, filesize) != 0){
            die("SERVER PUT","Connection closed while reading file for PUT");
        }
        /* save the file buffer to the server */
        FILE * fp = fopen(filename, "wb");
        fwrite(file, sizeof(file), 1, fp);
        fclose(fp);
        /* tell the client the PUT was successful */
        char response[] = "OK\n";
        Send_Int(connfd, sizeof(response));
        Send(connfd, response, sizeof(response));
    }
    /* handle GET */
    else{
        FILE * fp = fopen(filename, "rb");
        if(fp == NULL){
            send_error(connfd, "GET file not found\n");
            return;
        }
        fseek(fp, 0, SEEK_END);
        size_t filesize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        unsigned char file[filesize];
        size_t bytes_read = fread(file, sizeof(unsigned char), filesize, fp);
        if(bytes_read != filesize){
            fclose(fp);
            send_error(connfd, "GET file could not be read\n");
            return;
        }
        fclose(fp);
        
        /* convert filesize to string */
        int j = 1, temp_size = filesize / 10;
        while (temp_size)
        {
            j++;
            temp_size /= 10;
        }
        char size[j];
        sprintf(size, "%d", filesize); 

        uint32_t response_headersize = strlen("OK\n") + strlen(filename) + j + 2;  
        char response_header[response_headersize];
        bzero(response_header, response_headersize);
        strcpy(response_header, "OK\n");
        strcat(response_header, filename);
        strcat(response_header, "\n");
        strcat(response_header, size);
        strcat(response_header, "\n");

        /* send the header size */
        Send_Int(connfd, response_headersize);
        /* send the header */
        Send(connfd, response_header, response_headersize);
        /* send the file */
        Send(connfd, file, filesize);
    }
}
/*
 * file_server() - Read a request from a socket, satisfy the request, and
 *                 then close the connection.
 */
// void file_server(int connfd, int lru_size) {
//     /* TODO: set up a few static variables here to manage the LRU cache of
//        files */

//     /* TODO: replace following sample code with code that satisfies the
//        requirements of the assignment */
//     while(1) {
//         /* read the header information */
//         const int HEADERSIZE = 128;
//         char buffer[HEADERSIZE];
//         bzero(buffer, HEADERSIZE);
//         /* read from the socket, handle shortcounts */
//         char * buf_location = buffer;
//         ssize_t bytes_left = HEADERSIZE;
//         size_t bytes_sofar;
//         while(bytes_left > 0){
//             /* read data, swallow EINTRs */
//             if((bytes_sofar = read(connfd, buf_location, bytes_left)) < 0){
//                 if(errno != EINTR)
//                     die("read error: ", strerror(errno));
//                 continue;
//             }
//             //fprintf(stderr, "Bytes so far: %d\n", bytes_sofar);
//             /* entire file was less than HEADERSIZE */
//             if(bytes_sofar == 0){
//                 break;
//             }
//             /* update pointer */
//             buf_location += bytes_sofar;
//             bytes_left -= bytes_sofar;
//             /* this prevents a read hang when the size of the file is less than the assumed header size */
//             if(*(buf_location-1) == CUSTOM_EOF){
//                 break;
//             }
//         }
//         /* parse the header */
//         /* first line is PUT or GET */
//         char header[strlen(buffer)];
//         strcpy(header, buffer);
//         char * request_type = strtok(header, "\n");
//         int is_put = (strcmp(request_type, "PUT") == 0);
//         int is_get = (strcmp(request_type, "GET") == 0);
//         if(!is_put && !is_get){
//             send_error(connfd, "Request must begin with PUT or GET\n");
//             break;
//         }
//         /* next line is filename */
//         char * filename;
//         if((filename = strtok(NULL, "\n")) == NULL){
//             send_error(connfd, "Request must include filename\n");
//             break;
//         }
//         /* handle PUT */
//         if(is_put){
//             char * filesize_str;
//             if((filesize_str = strtok(NULL, "\n")) == NULL){
//                 send_error(connfd, "PUT request must include filesize\n");
//                 break;
//             }
//             char * t;
//             long filesize = strtol(filesize_str, &t, 10);
//             if(filesize_str == t){
//                 send_error(connfd, "Invalid filesize in PUT request\n");
//                 break;
//             }
//             fprintf(stderr, "\tRequest Type:\t%s\n\tFilename:\t%s\n\tFilesize:\t%d bytes\n", request_type, filename, filesize);
//             //fprintf(stderr, "The Buffer Is:\n%s\n", buffer);
//             /* copy the file data from the header buffer to the file buffer */
//             off_t offset = strlen(request_type) + strlen(filename) + strlen(filesize_str) + 3;
//             size_t already_read = HEADERSIZE - bytes_left - offset;
//             char file_buffer[filesize];
//             bzero(file_buffer, filesize);
//             memcpy(file_buffer, &buffer[offset], already_read);
//             //fprintf(stderr, "After memcpy it is: \n%s\n", file_buffer);
//             /* read the rest of the file, if there is any */
//             /* read from the socket, handle shortcounts */
//             buf_location = file_buffer;
//             buf_location += already_read;
//             bytes_left = filesize - already_read;
//             while(bytes_left > 0){
//                 /* read data, swallow EINTRs */
//                 if((bytes_sofar = read(connfd, buf_location, bytes_left)) < 0){
//                     if(errno != EINTR)
//                         die("read error: ", strerror(errno));
//                     continue;
//                 }
//                 /* this shouldn't ever happen... */
//                 if(bytes_sofar == 0){
//                     fprintf(stderr, "File Buffer so far: %s\n", file_buffer);
//                     send_error(connfd, "Entire file could not be read in PUT request\n");
//                     return;
//                 }
//                 /* update pointer */
//                 buf_location += bytes_sofar;
//                 bytes_left -= bytes_sofar;
//             }
//             /* save the file buffer to the server */
//             FILE * fp = fopen(filename, "wb");
//             fwrite(file_buffer, sizeof(file_buffer) + 1, 1, fp);
//             fclose(fp);
//             /* tell the client the PUT was successful */
//             write(connfd, "OK\n", 3);
//             break;
//         }
//         /* handle GET */
//         else{
//             //write(connfd, EOF, 1);
//             fprintf(stderr, "BREAKING FROM GET\n");
//             break;
//         }
//     }
//     /* sample code: continually read lines from the client, and send them
//        back to the client immediately */
//     // while (1) {
//     //     const int MAXLINE = 8192;
//     //     char      buf[MAXLINE];   /* a place to store text from the client */
//     //     bzero(buf, MAXLINE);

//     //     /* read from socket, recognizing that we may get short counts */
//     //     char *bufp = buf;              /* current pointer into buffer */
//     //     ssize_t nremain = MAXLINE;     /* max characters we can still read */
//     //     size_t nsofar;                 /* characters read so far */
//     //     while (1) {
//     //         /* read some data; swallow EINTRs */
//     //         if ((nsofar = read(connfd, bufp, nremain)) < 0) {
//     //             if (errno != EINTR)
//     //                 die("read error: ", strerror(errno));
//     //             continue;
//     //         }
//     //         /* end service to this client on EOF */
//     //         if (nsofar == 0) {
//     //             fprintf(stderr, "received EOF\n");
//     //             return;
//     //         }
//     //         /* update pointer for next bit of reading */
//     //         bufp += nsofar;
//     //         nremain -= nsofar;
//     //         if (*(bufp-1) == '\n') {
//     //             *bufp = 0;
//     //             break;
//     //         }
//     //     }

//     //     /* dump content back to client (again, must handle short counts) */
//     //     printf("server received %d bytes\n", MAXLINE-nremain);
//     //     nremain = strlen(buf);
//     //     bufp = buf;
//     //     while (nremain > 0) {
//     //         /* write some data; swallow EINTRs */
//     //         if ((nsofar = write(connfd, bufp, nremain)) <= 0) {
//     //             if (errno != EINTR)
//     //                 die("Write error: ", strerror(errno));
//     //             nsofar = 0;
//     //         }
//     //         nremain -= nsofar;
//     //         bufp += nsofar;
//     //     }
//     // }
// }

/*
 * main() - parse command line, create a socket, handle requests
 */
int main(int argc, char **argv) {
    /* for getopt */
    long opt;
    int  lru_size = 10;
    int  port     = 9000;

    check_team(argv[0]);

    /* parse the command-line options.  They are 'p' for port number,  */
    /* and 'l' for lru cache size.  'h' is also supported. */
    while ((opt = getopt(argc, argv, "hl:p:")) != -1) {
        switch(opt) {
          case 'h': help(argv[0]); break;
          case 'l': lru_size = atoi(argv[0]); break;
          case 'p': port = atoi(optarg); break;
        }
    }

    /* open a socket, and start handling requests */
    int fd = open_server_socket(port);
    handle_requests(fd, file_server, lru_size);

    exit(0);
}
