#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符，与从epoll中删除文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );


//信号处理函数，第二章信号捕捉那块，添加了一个信号捕捉
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}


//主函数，传入的参数至少要有端口号
int main( int argc, char* argv[] ) {
    
    //传入参数至少要有端口号
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    //把字符串转换为整数获得端口号
    int port = atoi( argv[1] );
    //对SIGPIPE信号进行处理，默认忽略
    addsig( SIGPIPE, SIG_IGN );


    //创建线程池，初始化线程池
    //http_conn是一个任务类
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    //创建数组，保存所有的客户端信息
    http_conn* users = new http_conn[ MAX_FD ];

    //建立TCP网络通信套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );


    // 端口复用，一定要在绑定之前完成，这里要用epoll
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    //绑定
    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    // 监听文件描述符添加到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    //循环检测是否有事件发生
    while(true) {
        
        //检测到了几个事件
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        //循环遍历事件数组
        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            //有新的客户端链接
            if( sockfd == listenfd ) {
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                //接收新的
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                //如果连接数满了
                if( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd);
                    continue;
                }

                //将新的客户数据初始化放到users数组中
                users[connfd].init( connfd, client_address);

            }
            //不是新的客户端，是原本的客户端出现异常。
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {

                users[sockfd].close_conn();

            } 
            //判断是否有读事件发生
            else if(events[i].events & EPOLLIN) {

                if(users[sockfd].read()) {
                    //一次性把所有数据都读完
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();
                }

            }  
            //写事件
            else if( events[i].events & EPOLLOUT ) {
                //一次性写完
                if( !users[sockfd].write() ) {
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}