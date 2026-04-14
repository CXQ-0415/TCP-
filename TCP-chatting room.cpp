#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <unordered_map>

#define MAX_EVENTS 1024
#define PORT 8888

// ========== 线程池 ==========
class ThreadPool {
public:
    ThreadPool(size_t num_threads) : stop(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers) worker.join();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

// ========== 全局聊天室状态 ==========
std::unordered_map<int, std::string> g_clients;   // fd -> 昵称
std::mutex g_clients_mutex;

void broadcast(const std::string& message, int exclude_fd = -1) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (auto& pair : g_clients) {
        if (pair.first != exclude_fd) {
            write(pair.first, message.c_str(), message.size());
        }
    }
}

void handle_client_message(int client_fd, const std::string& msg) {
    std::string trimmed = msg;
    if (!trimmed.empty() && trimmed.back() == '\n') trimmed.pop_back();
    if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();

    if (trimmed.find("NICK:") == 0) {
        std::string nick = trimmed.substr(5);
        size_t start = nick.find_first_not_of(" \t");
        if (start != std::string::npos) nick = nick.substr(start);
        else nick = "anonymous";

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            g_clients[client_fd] = nick;
        }
        std::string welcome = nick + " 加入了聊天室\n";
        broadcast(welcome, client_fd);
        std::cout << nick << " (fd=" << client_fd << ") 加入" << std::endl;
    } else {
        std::string nick;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(client_fd);
            if (it != g_clients.end()) nick = it->second;
            else nick = "未知";
        }
        std::string chat_msg = nick + ": " + trimmed + "\n";
        broadcast(chat_msg);
    }
}

void client_disconnect(int client_fd, int epoll_fd) {
    std::string nick;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        auto it = g_clients.find(client_fd);
        if (it != g_clients.end()) {
            nick = it->second;
            g_clients.erase(it);
        }
    }
    if (!nick.empty()) {
        std::string leave_msg = nick + " 离开了聊天室\n";
        broadcast(leave_msg);
        std::cout << nick << " (fd=" << client_fd << ") 离开" << std::endl;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        std::cerr << "socket创建失败" << std::endl;
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "绑定失败" << std::endl;
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 10) == -1) {
        std::cerr << "监听失败" << std::endl;
        close(listen_fd);
        return -1;
    }

    set_nonblocking(listen_fd);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll创建失败" << std::endl;
        close(listen_fd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    ThreadPool pool(4);
    std::cout << "聊天室服务器启动，端口 " << PORT << std::endl;

    struct epoll_event events[MAX_EVENTS];

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) break;

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd == -1) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            std::cerr << "accept失败" << std::endl;
                        }
                        break;
                    }
                    std::cout << "新客户端连接，fd = " << client_fd << std::endl;
                    set_nonblocking(client_fd);
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                }
            } else {
                char buffer[4096];
                int n = read(fd, buffer, sizeof(buffer) - 1);
                if (n <= 0) {
                    client_disconnect(fd, epoll_fd);
                } else {
                    buffer[n] = '\0';
                    pool.enqueue([fd, msg = std::string(buffer), epoll_fd] {
                        handle_client_message(fd, msg);
                    });
                }
            }
        }
    }

    close(listen_fd);
    close(epoll_fd);
    return 0;
}
