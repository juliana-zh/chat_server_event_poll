#include <iostream>
#include <algorithm>
#include <set>
#include <unordered_map>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <cstring>

#define MAX_EVENTS 32

int set_nonblock(int fd) {
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}

const unsigned BUF_SIZE = 1024;
using IP_addr = std::string;

void notify_others(const std::string& msg, const std::unordered_map<int, IP_addr>& slaveSockets) {
    static char BufferToNotifyAboutNewClient[BUF_SIZE];
    strcpy(BufferToNotifyAboutNewClient, msg.c_str());
    
    for (const auto& el : slaveSockets) {
        send(el.first, 
        BufferToNotifyAboutNewClient,
        msg.size(),
        MSG_NOSIGNAL);
    }
}

int main(int argc, char** argv) {
    std::unordered_map<int, IP_addr> slaveSockets;

    int MasterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (MasterSocket == -1) {
        perror("cannot create socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in SockAddr;
    SockAddr.sin_family = AF_INET;
    SockAddr.sin_port = htons(12345);
    SockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    int opt = 1;
    setsockopt(MasterSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(MasterSocket, (struct sockaddr *)(&SockAddr), sizeof(SockAddr)) == -1) {
        perror("bind failed");
        close(MasterSocket);
        exit(EXIT_FAILURE);
    }

    set_nonblock(MasterSocket);

    if (listen(MasterSocket, SOMAXCONN) == -1) {
        perror("listen failed");
        close(MasterSocket);
        exit(EXIT_FAILURE);
    }

    int EPoll = epoll_create1(0);

    struct epoll_event Event;
    Event.data.fd = MasterSocket;
    Event.events = EPOLLIN;
    epoll_ctl(EPoll, EPOLL_CTL_ADD, MasterSocket, &Event);

    while (true) {
        struct epoll_event Events[MAX_EVENTS];
        int N = epoll_wait(EPoll, Events, MAX_EVENTS, -1);
        for(unsigned int i = 0; i < N; ++i) {
            if (Events[i].data.fd == MasterSocket) {
                struct sockaddr_in SockAddr;
                socklen_t slen = sizeof(SockAddr);
                int SlaveSocket = accept(MasterSocket, (struct sockaddr*)&SockAddr, &slen);
                if (SlaveSocket <= 0) {
                    printf("accept failed");
                    continue;
                }
                char *slave_ip = inet_ntoa(SockAddr.sin_addr);

                static const std::string msg = 
                    "A new client (IP=" + std::string(slave_ip) + ") connected. \n";
                notify_others(msg, slaveSockets);

                slaveSockets.insert({SlaveSocket, slave_ip});                
                set_nonblock(SlaveSocket);

                struct epoll_event Event;
                Event.data.fd = SlaveSocket;
                Event.events = EPOLLIN;
                epoll_ctl(EPoll, EPOLL_CTL_ADD, SlaveSocket, &Event);
            } else {
                 static char Buffer[BUF_SIZE];
                 int RecvResult = recv(Events[i].data.fd, Buffer, BUF_SIZE, MSG_NOSIGNAL);

                 if ((RecvResult == 0) && (errno != EAGAIN)) {
                    shutdown(Events[i].data.fd, SHUT_RDWR);
                    close(Events[i].data.fd);

                    auto it = slaveSockets.find(Events[i].data.fd);
                    if (it == slaveSockets.end()) {
                        perror("client with unknown desc disconnected");
                        continue;
                    }
                    static const std::string msg = 
                        "Client (IP=" + std::string(it->second) + ") disconnected. \n";
                    notify_others(msg, slaveSockets);

                 } else if (RecvResult > 0) {
                    for (const auto& el : slaveSockets) {
                        if (el.first == Events[i].data.fd) {
                            continue;
                        }
                        IP_addr slaveIpAddr = el.second;
                        std::string delim = ": ";
                        slaveIpAddr += delim;
                        static char BufferToSend[100 + BUF_SIZE];

                        strcpy(BufferToSend, slaveIpAddr.c_str());
                        strcpy(BufferToSend + slaveIpAddr.size(), Buffer);

                        if (BufferToSend[slaveIpAddr.size() + RecvResult - 1] != '\n') {
                            char end = '\n';
                            strcpy(BufferToSend + slaveIpAddr.size() + RecvResult, &end);
                        }

                        send(el.first, 
                        BufferToSend,
                        slaveIpAddr.size() + RecvResult + 1,
                        MSG_NOSIGNAL);
                    }
                 }
            }
        }
    }

    return 0;
}