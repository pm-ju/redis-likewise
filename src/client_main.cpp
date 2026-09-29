#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

int main(int argc, char* argv[]) {
    if (argc != 1 && argc != 3) { std::cerr << "Usage: kv_client [address port]\n"; return 2; }
    const std::string address = argc == 3 ? argv[1] : "127.0.0.1";
    const std::string port = argc == 3 ? argv[2] : "6380";
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) { std::cerr << "Failed to create socket: " << std::strerror(errno) << '\n'; return 1; }
    sockaddr_in endpoint{}; endpoint.sin_family = AF_INET; endpoint.sin_port = htons(static_cast<std::uint16_t>(std::stoi(port)));
    if (inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1 || connect(socket_fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        std::cerr << "Failed to connect to " << address << ':' << port << '\n'; close(socket_fd); return 1;
    }
    std::cout << "Connected to " << address << ':' << port << "\n\n";
    std::string command;
    char response[4096];
    while (std::cout << "> " && std::getline(std::cin, command)) {
        command += '\n';
        if (send(socket_fd, command.data(), command.size(), MSG_NOSIGNAL) < 0) break;
        const ssize_t received = recv(socket_fd, response, sizeof(response) - 1, 0);
        if (received <= 0) break;
        response[received] = '\0'; std::cout << response << '\n';
        if (command == "QUIT\n" || command == "quit\n") break;
    }
    close(socket_fd);
    return 0;
}
