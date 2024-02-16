#include <asm-generic/errno-base.h>
#include <liburing/io_uring.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>

#define QD 6
#define BS 0x400
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
    struct __kernel_timespec timeout_ts = {0};
    timeout_ts.tv_sec = 1;

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

    events = ready = 0;
    signal(SIGABRT, cleanup);
    ret = io_uring_queue_init(QD, &ring, 0);
    if(ret < 0){
        LOG_ERR("io_uring_queue_init");
        exit(1);
    }

    while(1){
        if(events >= QD){
            if(!ready){
                ret = io_uring_wait_cqe(&ring, &cqe);
                ready = 1;
            } else {
                ret = io_uring_peek_cqe(&ring, &cqe);
                if(ret == -EAGAIN) {
                    cqe = NULL;
                    ret = 0;
                }
            }
            if(ret < 0){
                LOG_ERR("io_uring_peek_cqe");
                cleanup(SIGABRT);
            }
            if(!cqe){
                usleep(500);
                continue;
            }
            data = io_uring_cqe_get_data(cqe);
            if(cqe->res < 0){
                if(cqe->res == -EAGAIN){
                    //requeue
                    queue(listenfd, BS);
                    io_uring_cqe_seen(&ring, cqe);
                    continue;
                } else {
                    printf("[!] io_uring_cqe: %s\n", strerror(-cqe->res));
                    cleanup(SIGABRT);
                }
            }
            free(data);
            events -= 3;
            io_uring_cqe_seen(&ring, cqe);
        }
        
        queue(listenfd, BS);
        events += 3;
    }
}

void sender(int writefd){
    char* buffer;
    buffer = malloc(BS);
    unsigned long bytes_read;
    if(!buffer){
        LOG_ERR("malloc");
        return;
    }
    for(;;){
        bytes_read = read(STDIN_FILENO, buffer, BS);
        if(bytes_read == -1)
            goto out;
        if(!strcmp(buffer, "exit\n"))
            goto out;
        write(writefd, buffer, bytes_read);
    }

out:
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
