#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

#define LOG_ERR(msg) printf("[!][%s:%d] %s\n", __FILE__, __LINE__, msg);
#define MIN_PORT 1000
#define MAX_PORT 65535
#define QD 8
#define BS 0x400
#define RECV 1
#define WRITE 0

struct io_data{
    uint64_t type;
    struct iovec iov;
    char data[];
};

int setup_server(short port, int* serv_sock, int* client_sock){
    int ret;
    struct sockaddr_in server, client;
    socklen_t client_len;

    ret = -1;
    *serv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if(*serv_sock < 0){
        LOG_ERR("socket");
        goto out;
    }

    memset(&server, 0 , sizeof(server)); 

    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_port = htons(port);
    server.sin_family = AF_INET;

    ret = bind(*serv_sock, (struct sockaddr*)&server, sizeof(server));
    if(ret == -1){
        LOG_ERR("bind");
        goto out;
    }

    ret = listen(*serv_sock, 1);
    if(ret == -1){
        LOG_ERR("listen");
        goto out;
    }

    client_len = sizeof(client);

    *client_sock = accept(*serv_sock, (struct sockaddr*)&client, &client_len);
    if(*client_sock < 0){
        LOG_ERR("accept");
        printf("return value: %d\n", *client_sock);
        goto out;
    }

    ret = 0;
out:
    return ret;
}

int queue_write(struct io_data* data, int log_fd, uint64_t offset, struct io_uring* ring){
    struct io_uring_sqe* sqe;
    int ret;

    sqe = io_uring_get_sqe(ring);
    if(!sqe){
        LOG_ERR("io_uring_get_sqe");
        goto out;
    }

    io_uring_prep_writev(sqe, log_fd, &data->iov, 1, offset);
    io_uring_sqe_set_data(sqe, data);
    io_uring_submit(ring);

    ret = 0;
out:
    return ret;
}

int queue_recv(int client_sock, struct io_uring* ring){
    struct io_uring_sqe* sqe;
    struct io_data* data;
    int ret;

    ret = -1;
    data = malloc(sizeof(struct io_data) + BS);
    if(data < 0){
        LOG_ERR("malloc");
        goto out;
    }

    data->iov.iov_len = BS;
    data->iov.iov_base = &data->data;
    data->type = RECV;

    sqe = io_uring_get_sqe(ring);
    if(!sqe){
        LOG_ERR("io_uring_get_sqe");
        goto out;
    }

    io_uring_prep_recv(sqe, client_sock, &data->data, BS, 0);
    io_uring_sqe_set_data(sqe, data);

    io_uring_submit(ring);
    ret = 0;
out:
    return ret;
}

void handle_connection(int serv_sock, int client_sock, int log_fd){
    struct io_uring ring;
    struct io_uring_cqe* cqe;
    struct io_data* data;
    uint64_t bytes_written;
    uint8_t recvs, writes;
    uint64_t i;
    recvs = writes = 0;
    bytes_written = 0;

    io_uring_queue_init(QD, &ring, 0);
    //fill queue
    for(i = 0; i < QD; i++){
        queue_recv(client_sock, &ring);
        recvs++;
    }

    for(;;){
        io_uring_wait_cqe(&ring, &cqe);
        data = io_uring_cqe_get_data(cqe);
        switch(data->type){
            case RECV:
                if(cqe->res == -1){
                    printf("[*] error when recieving\n");
                    goto out;
                } else if(cqe->res == 0) {
                    printf("[*] nothing recieved exiting gracefully\n");
                    goto out;
                }
                data->type = WRITE;
                data->iov.iov_len = cqe->res;
                io_uring_cqe_seen(&ring, cqe);
                queue_write(data, log_fd, bytes_written, &ring);
                bytes_written += cqe->res;
                recvs--;
                writes++;
                break;
            case WRITE:
                if(cqe->res < 0){
                    LOG_ERR("write io_uring_request error");
                    goto out;
                }
                free(data);
                io_uring_cqe_seen(&ring, cqe);
                queue_recv(client_sock, &ring);
                recvs++;
                writes--;
                break;
            default:
                LOG_ERR("invalid data type");
                goto out;
        }
    }

out:
    io_uring_queue_exit(&ring);
    close(serv_sock);
    close(client_sock);
    close(log_fd);
}

int main(int argc, char* argv[]){
    int serv_sock, client_sock, port, log_fd;
    int ret = -1;

    if(argc < 3){
        printf("[*] USAGE: ./server <PORT> <LOG FILE>\n");
        exit(0);
    }

    port = atoi(argv[1]);

    if(port < MIN_PORT || port > MAX_PORT){
        printf("[!] please choose a port between 1000-65535\n");
        exit(0);
    }

    if((ret = setup_server((short)port, &serv_sock, &client_sock)) == -1){
        LOG_ERR("Failed to create server socket");
        exit(1);
    }

    log_fd = open(argv[2], O_RDWR | O_CREAT, 0644);
    if(log_fd < 0){
        LOG_ERR("open");
        exit(1);
    }

    handle_connection(serv_sock, client_sock, log_fd);
}
