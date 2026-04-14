# TCP 多人聊天室服务器
基于 Linux epoll + 线程池 实现的高并发 TCP 多人聊天室

## 功能介绍
- 多客户端同时在线聊天
- 支持设置昵称（NICK:xxx）
- 消息广播（一人发送，全员接收）
- 高并发：epoll ET 模式
- 线程池异步处理消息
- 自动上线/下线通知

## 技术栈
- C++
- epoll IO多路复用
- 线程池
- 多线程
- TCP Socket
- Linux 系统编程

## 编译运行
```bash
g++ -o TCP-ChatRoom TCP-ChatRoom.cpp -pthread
./TCP-ChatRoom

使用方法：
ip：127.0.0.1
端口：8888
