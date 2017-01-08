/*
 Copyright (c) 2016 waynezxcv <liuweiself@126.com>
 
 https://github.com/waynezxcv/DispatchSocket
 
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */

#include "TCPSocket.hpp"

using namespace DispatchSocket;

const int kMaxConnectCount = 32;

#pragma mark - LifeCycle

TCPSocket::TCPSocket() : _sockFd(LW_SOCK_NULL) ,_addressFamily(LW_SOCK_NULL) {
    _sockQueue = dispatch_queue_create("com.waynezxcv.DispatchSocket.sockQueue", DISPATCH_QUEUE_SERIAL);
    _connectedSockets = std::vector<TCPSocket*>();
}

TCPSocket::~TCPSocket() {
    dispatch_release(_sockQueue);
    shutdown();
}

#pragma mark - Listen

bool TCPSocket::sockListen() {
    return TCPSocket::sockListen(0);
}

bool TCPSocket::sockListen(const uint16_t &port) {
    auto creatSock = [](int domain,const struct sockaddr* addr) -> int {
        //Get socket file descriptor
        int sockFd = socket(domain, SOCK_STREAM, 0);
        if (LW_SOCK_NULL == sockFd) {
            return LW_SOCK_NULL;
        }
        int status;
        //non block
        status = fcntl(sockFd,F_SETFL,O_NONBLOCK);
        if (-1 == status) {
            close(sockFd);
            return LW_SOCK_NULL;
        }
        
        //reuse address
        int reuseOn = 1;
        status = setsockopt(sockFd,SOL_SOCKET,SO_REUSEADDR,&reuseOn,sizeof(reuseOn));
        if (-1 == status) {
            close(sockFd);
            return LW_SOCK_NULL;
        }
        
        //no signal pipe
        int nosigpipe = 1;
        status = setsockopt(sockFd,SOL_SOCKET,SO_NOSIGPIPE,&nosigpipe,sizeof(nosigpipe));
        
        if (-1 == status) {
            close(sockFd);
            return LW_SOCK_NULL;
        }
        
        //Bind
        status = bind(sockFd, addr, sizeof(*addr));
        if (-1 == status) {
            close(sockFd);
            return LW_SOCK_NULL;
        }
        
        //Listen
        status = listen(sockFd, kMaxConnectCount);
        if (-1 == status) {
            close(sockFd);
            return LW_SOCK_NULL;
        }
        return sockFd;
    };
    
    struct sockaddr sockaddr;
    AddressHelper::getSockaddrStruct("", port, &sockaddr);
    
    if (AddressHelper::isIPv4Addr(&sockaddr)) {
        _addressFamily = AF_INET;
        _sockFd = creatSock(AF_INET,&sockaddr);
    } else if (AddressHelper::isIPv6Addr(&sockaddr)) {
        _addressFamily = AF_INET6;
        _sockFd = creatSock(AF_INET6,&sockaddr);
    }
    if (_sockFd == LW_SOCK_NULL) {
#ifdef DEBUG
        std::cout<<"create listen socket failed!"<<std::endl;
#endif
        return false;
    }
    
#ifdef DEBUG
    uint16_t p;
    std::string host;
    sockGetSockName(_sockFd,host, p);
    std::cout<<"server start listen!  listenFd:"<<_sockFd<<std::endl;
    std::cout<<"host:"<<sockGetIfaddrs()<<std::endl;
    std::cout<<"port:"<<p<<std::endl;
#endif
    
    //accept source
    __unsafe_unretained TCPSocket* weakThis = this;
    dispatch_source_t acceptSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
                                                            _sockFd,
                                                            0,
                                                            _sockQueue);
    //event handler
    dispatch_source_set_event_handler(acceptSource, ^{
        TCPSocket* strongThis = weakThis;
        if (strongThis == nullptr) {
            return;
        }
        unsigned long pending = dispatch_source_get_data(_accpetSource);
        if (pending > 0 && _connectedSockets.size() < kMaxConnectCount ) {
            struct sockaddr c_addr;
            AddressHelper::getSockaddrStruct(sockGetIfaddrs(), p, &c_addr);
            weakThis->acceptHandler(_sockFd,AddressHelper::getUrl(&c_addr));
        }
    });
    
    dispatch_resume(acceptSource);
    _accpetSource = acceptSource;
    return (LW_SOCK_NULL != _sockFd);
}

#pragma mark - Accept

void TCPSocket::acceptHandler(const int& fd,const std::string& url) {
    
    struct sockaddr* sockaddr = nullptr;
    socklen_t addrLen;
    
    if (_addressFamily == AF_INET) {
        struct sockaddr_in addr_in;
        addrLen = sizeof(addr_in);
        sockaddr = (struct sockaddr *)&addr_in;
        
    } else if(_addressFamily == AF_INET6) {
        
        struct sockaddr_in6 addr_in6;
        addrLen = sizeof(addr_in6);
        sockaddr = (struct sockaddr *)&addr_in6;
    }
    
    int connFd = accept(fd,sockaddr, &addrLen);
    if (connFd == LW_SOCK_NULL) {
#ifdef DEBUG
        printf("accept fail!\n");
#endif
        return;
    }
    
    TCPSocket* connectSocket = new TCPSocket();
    connectSocket->setSockFd(connFd);
    connectSocket->setAddressFamily(_addressFamily);
    //在新创建Sokect对象的sockeQueue中添加read/writeSource
    dispatch_async(connectSocket->getSockQueue(), ^{
        connectSocket->setupReadAndWriteSource(connFd, url);
    });
    _connectedSockets.push_back(connectSocket);
    
#ifdef DEBUG
    std::string clientAddr = AddressHelper::getUrl(sockaddr);
    std::cout<<"*** *** *** *** ***"<<std::endl;
    std::cout<<"accept!  connect Fd:"<<connFd<<std::endl;
    std::cout<<"client address:"<<clientAddr<<std::endl;
    std::cout<<"*** *** *** *** ***"<<std::endl;
#endif
}



void TCPSocket::setupReadAndWriteSource(const int& connFd,const std::string& url) {
    /******************************* Read *************************************/
    dispatch_source_t readSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,connFd,0,_sockQueue);
    __unsafe_unretained TCPSocket* weakThis = this;
    dispatch_source_set_event_handler(readSource, ^{
        TCPSocket* strongThis = weakThis;
        if (strongThis == nullptr) {
            return ;
        }
        
        //获取read source中可用的字节数
        size_t available = dispatch_source_get_data(readSource);
        if (available <= 0) {
            if (_currentRead) {
                printf("currentRead Lenth:%d\n",_currentRead->_length);
                _currentRead = NULL;
                printf("===== reset buffer ====== \n");
                
            }
            return;
        }
        
        printf("availabel:%zu\n",available);
        
        char buffer[kBufferSize];
        ssize_t length = 0;
        
        if (_currentRead == NULL) {
            _currentRead = std::make_shared<SocketDataPacket>();
        }
        
        
        if (available > kBufferSize) {
            length = read(connFd, buffer, kBufferSize);//32768
            
            if (length < 0) {
#ifdef DEBUG
                printf("read error!\n");
#endif
                dispatch_source_cancel(readSource);
                return;
            }
            //append
            _currentRead->appendBuffer(buffer,length);
            printf("read size :%zd\n",length);
            printf("current buffer size:%zd\n",_currentRead->_length);
            
        } else {
            
            length = read(connFd, buffer, available);
            
            if (length < 0) {
#ifdef DEBUG
                printf("read error!\n");
#endif
                dispatch_source_cancel(readSource);
                return;
            }
            
            else if (length > 0) {
                _currentRead->appendBuffer(buffer,length);
                printf("read size :%zd\n",length);
                printf("current buffer size:%zd\n",_currentRead->_length);
                printf("currentRead Lenth:%d\n",_currentRead->_length);
                _currentRead = NULL;
                printf("===== reset buffer ====== \n");
            }
            
            else {
                printf("currentRead Lenth:%d\n",_currentRead->_length);
                _currentRead = NULL;
                
            }
        }
    });
    
    //read source 取消处理
    dispatch_source_set_cancel_handler(readSource, ^{
        dispatch_release(readSource);
    });
    
    /******************************* Write *************************************/
    dispatch_source_t writeSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE,connFd,0,_sockQueue);
    dispatch_source_set_event_handler(writeSource, ^{
        
        
    });
    
    //write source 取消处理
    dispatch_source_set_cancel_handler(writeSource, ^{
        dispatch_release(writeSource);
    });
    
    //启动resource
    dispatch_resume(readSource);
    dispatch_resume(writeSource);
}


#pragma mark - Connect

bool TCPSocket::sockConnect(const std::string &host, const uint16_t &port) {
    struct sockaddr sockaddr;
    auto createSock = [](struct sockaddr* sockaddr,const std::string& host,const uint16_t& port) -> int {
        int connFd = LW_SOCK_NULL;
        AddressHelper::getSockaddrStruct(host, port, sockaddr);
        if (AddressHelper::isIPv4Addr(sockaddr)) {
            connFd = socket(AF_INET, SOCK_STREAM, 0);
        } else if (AddressHelper::isIPv6Addr(sockaddr)) {
            connFd = socket(AF_INET6, SOCK_STREAM, 0);
        }
        if (connFd == LW_SOCK_NULL) {
#ifdef DEBUG
            std::cout<<"connect fail!"<<std::endl;
#endif
            return false;
        }
        //Connect
        int result = connect(connFd,sockaddr, sockaddr -> sa_len);
        if (LW_SOCK_NULL == result) {
            close(connFd);
            return LW_SOCK_NULL;
        }
        
        //non block
        int status;
        status = fcntl(connFd,F_SETFL,O_NONBLOCK);
        if (LW_SOCK_NULL == status) {
            close(connFd);
            return LW_SOCK_NULL;
        }
        
        //reuse address
        int reuseOn = 1;
        status = setsockopt(connFd,SOL_SOCKET,SO_REUSEADDR,&reuseOn,sizeof(reuseOn));
        if (LW_SOCK_NULL == status) {
            close(connFd);
            return LW_SOCK_NULL;
        }
        
        //no signal pipe
        int nosigpipe = 1;
        status = setsockopt(connFd,SOL_SOCKET,SO_NOSIGPIPE,&nosigpipe,sizeof(nosigpipe));
        if (LW_SOCK_NULL == status) {
            close(connFd);
            return LW_SOCK_NULL;
        }
        
        return connFd;
    };
    
    int connFd = createSock(&sockaddr,host,port);
    if (connFd == LW_SOCK_NULL) {
#ifdef DEBUG
        std::cout<<"connect fail!\n"<<std::endl;
        return false;
#endif
    }
    _sockFd = connFd;
    if (AddressHelper::isIPv4Addr(&sockaddr)) {
        _addressFamily = AF_INET;
    } else {
        _addressFamily = AF_INET6;
    }
    
    dispatch_async(_sockQueue, ^{
        setupReadAndWriteSource(_sockFd, AddressHelper::getUrl(&sockaddr));
    });
    
    
#ifdef DEBUG
    std::cout<<"*** *** *** *** ***"<<std::endl;
    std::cout<<"client connected! ===  sockFd:"<<_sockFd<<std::endl;
    std::cout<<"host  "<<AddressHelper::getUrl(&sockaddr)<<std::endl;
    std::cout<<"*** *** *** *** ***"<<std::endl;
#endif
    return (LW_SOCK_NULL != _sockFd);
}

bool TCPSocket::sockDisconnect() {
    return sockClose(_sockFd);
}

#pragma mark - I/O

ssize_t TCPSocket::sockRead(int fd, void* buffer,size_t length) {
    return 0;
}

ssize_t TCPSocket::sockWrite(int fd, void* buffer,size_t length) {
    return 0;
}

#pragma mark - close

bool TCPSocket::sockClose(const int& fd) {
    return (close(fd) != LW_SOCK_NULL);
}

void TCPSocket::shutdown() {
    sockClose(_sockFd);
    for (auto i = _connectedSockets.begin(); i != _connectedSockets.end(); ++ i) {
        TCPSocket* tcpSocket = *i;
        delete tcpSocket;
    }
    _connectedSockets.clear();
}

#pragma mark - Setter & Getter

int TCPSocket::getSockFd() const {
    return _sockFd;
}

void TCPSocket::setSockFd (const int& fd) {
    _sockFd = fd;
}

void TCPSocket::setAddressFamily(const int& af) {
    _addressFamily = af;
}

int TCPSocket::getSockAddressFamily() const {
    return _addressFamily;
}

dispatch_queue_t TCPSocket::getSockQueue() const {
    return _sockQueue;
}

#pragma mark - Others

int TCPSocket::currentConnectedSocketsCount() const {
    return static_cast<int>(_connectedSockets.size());
}

