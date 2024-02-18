#include <asm-generic/errno-base.h>
#include <netinet/in.h>
#include <liburing/io_uring.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <arpa/inet.h>

#define QD 6
#define BS 0x400
#define PORT 9001

#define LOG_ERR(msg) printf("[!][%s:%d] %s\n", __FILE__, __LINE__, msg);

struct io_uring ring;
int fds[2]; //read write

struct io_data {
    int read;
    off_t size;
    struct iovec iov;
};

void cleanup(int signum){
    printf("[*] exitting reciever\n");
    close(fds[0]);
    io_uring_queue_exit(&ring);    
    exit(0);
}

void queue(int fd, off_t size){
    struct io_data* data;
    struct io_uring_sqe* sqe;
    data = malloc(sizeof(struct io_data) + size);

    //read
    sqe = io_uring_get_sqe(&ring);
    if(!sqe) {
        LOG_ERR("io_uring_get_sqe");
        cleanup(SIGABRT);
    }

    data->read = 1;
    data->size = size;
    data->iov.iov_len = size;
    data->iov.iov_base = data + 1;

    io_uring_prep_read(sqe, fd, &data->iov, 1, 0);
    io_uring_sqe_set_data(sqe, data);
    sqe->flags |= IOSQE_IO_LINK;

    //timeout
    //sqe = io_uring_get_sqe(&ring);
    //if(!sqe) {
    //    LOG_ERR("io_uring_get_sqe");
    //    cleanup(SIGABRT);
    //}
    //io_uring_prep_timeout(sqe, &timeout_ts, 1, 0);
    //sqe->flags |= IOSQE_IO_LINK;

    //write
    sqe = io_uring_get_sqe(&ring);
    if(!sqe) {
        LOG_ERR("io_uring_get_sqe");
        cleanup(SIGABRT);
    }

    io_uring_prep_write(sqe, STDOUT_FILENO, &data->iov, 1, 0);
    io_uring_sqe_set_data(sqe, data);

    io_uring_submit(&ring);
}

void reciever(int listenfd){
    int events;
    int ret;
    int ready;
    struct io_data* data;
    struct io_uring_cqe* cqe;
    unsigned int head;

    events = ready = 0;
    close(STDIN_FILENO);
    signal(SIGABRT, cleanup);
    ret = io_uring_queue_init(QD, &ring, 0);
    if(ret < 0){
        LOG_ERR("io_uring_queue_init");
        exit(1);
    }

    while(1){
        if(events >= QD){
            ret = io_uring_wait_cqe(&ring, &cqe);
            io_uring_for_each_cqe(&ring, head, cqe){
                data = io_uring_cqe_get_data(cqe);
                if(cqe->res < 0){
                    if(cqe->res == -EAGAIN){
                        //requeue
                        printf("[*] cqe EAGAIN");
                        queue(listenfd, BS);
                        io_uring_cqe_seen(&ring, cqe);
                        continue;
                    } else {
                        printf("[!] io_uring_cqe: %s\n", strerror(-cqe->res));
                        cleanup(SIGABRT);
                    }
                }
                io_uring_cqe_seen(&ring, cqe);
            }
            free(data);
            events -= 2;
        }
        
        queue(listenfd, BS);
        events += 2;
    }
}

void sender(int writefd){
    char* buffer;
    int serv_fd, client_fd;
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len;
    
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    buffer = malloc(BS);
    unsigned long bytes_read;
    if(!buffer){
        LOG_ERR("malloc");
        return;
    }
    serv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(serv_fd < 0){
        LOG_ERR("socket");
        return;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(PORT);
    bind(serv_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    listen(serv_fd, 1);

    client_len = sizeof(client_addr);
    client_fd = accept(serv_fd, (struct sockaddr*)&client_addr, &client_len);

    for(;;){
        bytes_read = recv(client_fd, buffer, BS, 0);
        if(bytes_read == -1)
            goto out;
        if(strcmp(buffer, "exit") == 0)
            goto out;
        write(writefd, buffer, bytes_read);
    }

out:
    close(client_fd);
    close(serv_fd);
    free(buffer);
    return;
}

int main(void){
    pid_t child;
    if(pipe(fds) == -1){
        LOG_ERR("pipe");
        exit(1);
    }
    child = fork();
    if(child == -1) {
        LOG_ERR("fork");
        exit(1);
    }

    if(child == 0) {
        close(fds[1]); //close the write end
        reciever(fds[0]);
    } else {
        close(fds[0]);
        sender(fds[1]);
        kill(child, SIGABRT); //close the reciever on sender exit
    }
    return 0;
}
