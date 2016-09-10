//
// Created by mabaiming on 16-8-29.
//

#include "tcp_server.h"

#include "common.h"
#include "logger.h"
#include "tunnel_package.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <map>
#include <set>

using namespace std;
using namespace Common;

void
TcpServer::init(const string& tunnelIp, uint16_t tunnelPort, int tunnelConnection,
    const string& trafficIp, const vector<uint16_t>& trafficPortList,
    int trafficConnection) {
  prepareTunnel(tunnelIp, tunnelPort, tunnelConnection);
  for (int i = 0; i < trafficPortList.size(); ++i) {
    prepareTraffic(trafficIp, trafficPortList[i], trafficConnection);
  }
}

int
TcpServer::assignTunnelClient(int trafficServerFd, int trafficClientFd) {
  if (tunnelClientMap.empty()) {
    log_warn << "tunnelClientMap is empty to assign";
    return -1;
  }
  map<int, int>::iterator it = trafficServerMap.find(trafficServerFd);
  if (it != trafficServerMap.end() && it->second > 0) {
    log_debug << "use assigned fd: " << it->second;
    return it->second;
  }

  int avg = trafficServerMap.size() / tunnelClientMap.size();
  map<int, TunnelClientInfo>::iterator it2 = tunnelClientMap.begin();
  for (; it2 != tunnelClientMap.end(); ++it2) {
    if (it2->second.count <= avg) {
      trafficServerMap[trafficServerFd] = it2->first;
      it2->second.count++;
      return it2->first;
    }
  }
  log_error << "cannot find one tunnelClient, it is impossible!!";
}

void
TcpServer::cleanUpTrafficClient(int fd) {
  map<int, int>::iterator it = trafficClientMap.find(fd);
  if (it != trafficClientMap.end()) {
    sendTunnelState(it->second, fd, TunnelPackage::STATE_CLOSE);
  }
  trafficClientMap.erase(fd);
  cleanUpFd(fd);
  log_debug << "clean up trafficClient, fd: " << fd;
}

void
TcpServer::cleanUpTunnelClient(int fd) {
  for (map<int, int>::iterator it = trafficServerMap.begin(); it != trafficServerMap.end(); ++it) {
    if (it->second == fd) {
      it->second = -1; // keep trafficServer free
    }
  }
  for (map<int, int>::iterator it = trafficClientMap.begin(); it != trafficClientMap.end();) {
    if (it->second == fd) {
      cleanUpFd(it->first);
      trafficClientMap.erase(it++);
    } else {
      ++it;
    }
  }
  cleanUpFd(fd);
  tunnelClientMap.erase(fd);
  log_debug << "clean up tunnelClient, fd: " << fd;
}

int
TcpServer::prepareTraffic(const string& ip, uint16_t port, int connection) {
  int fd = prepare(ip, port, connection);
  trafficServerMap[fd] = -1;
  return fd;
}

int
TcpServer::prepareTunnel(const string& ip, uint16_t port, int connection) {
  tunnelServerFd = prepare(ip, port, connection);
  return tunnelServerFd;
}


int
TcpServer::acceptTrafficClient(int serverFd) {
  struct sockaddr_in addr;
  socklen_t sin_size = sizeof(addr);
  int clientFd = accept(serverFd, (struct sockaddr *)&addr, &sin_size);
  if(clientFd < 0) {
    log_error << "failed to accept client";
    exit(EXIT_FAILURE);
  }
  log_info << "accept client, ip: " << inet_ntoa(addr.sin_addr) << ", port: " << addr.sin_port;

  int tunnelClientFd = assignTunnelClient(serverFd, clientFd);
  if (tunnelClientFd < 0) {
    cleanUpTrafficClient(clientFd);
    return -1;
  }
  trafficClientMap[clientFd] = tunnelClientFd;
  sendTunnelState(tunnelClientFd, clientFd, TunnelPackage::STATE_CREATE);

  registerFd(clientFd);
  return clientFd;
}

int
TcpServer::acceptTunnelClient(int serverFd) {
  struct sockaddr_in addr;
  socklen_t sin_size = sizeof(addr);
  int clientFd = accept(serverFd, (struct sockaddr *)&addr, &sin_size);
  if(clientFd < 0) {
    log_error << "failed to accept client";
    exit(EXIT_FAILURE);
  }
  log_info << "accept client, ip: " << inet_ntoa(addr.sin_addr) << ", port: " << addr.sin_port;

  tunnelClientMap[clientFd] = TunnelClientInfo(inet_ntoa(addr.sin_addr), addr.sin_port);

  registerFd(clientFd);
  return clientFd;
}

bool
TcpServer::handleTunnelClient(const struct epoll_event& event) {
  map<int, TunnelClientInfo>::iterator it = tunnelClientMap.find(event.data.fd);
  if (it == tunnelClientMap.end()) {
    return false;
  }
  if ((event.events & EPOLLRDHUP) || (event.events & EPOLLERR)) {
    cleanUpTunnelClient(event.data.fd);
    return true;
  }
  if ((event.events & EPOLLIN) == 0) {
    return true;
  }
  char buf[BUFFER_SIZE];
  int len = recv(event.data.fd, buf, BUFFER_SIZE, 0);
  if (len <= 0) {
    cleanUpTunnelClient(event.data.fd);
    return true;
  }
  it->second.buffer.append(buf, len);
  int offset = 0;
  int totalLength = it->second.buffer.length();
  while (offset < it->second.buffer.size()) {
    TunnelPackage package;
    int decodeLength = package.decode(it->second.buffer.c_str() + offset, totalLength - offset);
    if (decodeLength == 0) { // wait next time to read
      break;
    }
    offset += decodeLength;
    log_debug << "recv, trafficServer --> tunnelClient -[fd=" << package.fd
              << ",state=" << package.getState() << ",length=" << package.message.size()
              << "]-> *tunnelServer(" << event.data.fd << ") --> trafficClient";
    switch (package.state) {
      case TunnelPackage::STATE_HEARTBEAT: break;
      case TunnelPackage::STATE_CREATE_FAILURE:
      case TunnelPackage::STATE_CLOSE: cleanUpTrafficClient(package.fd); break;
      case TunnelPackage::STATE_TRAFFIC: {
        send(package.fd, package.message.c_str(), package.message.size(), 0);
        log_debug << "trafficServer --> tunnelClient --> tunnelServer(" << event.data.fd
                  << ") -[length=" << package.message.size() << "]-> trafficClient(" << package.fd << ")";
      } break;
      default: log_warn << "ignore state: " << (int) package.state;
    }
  }
  if (offset > 0) {
    it->second.buffer.assign(it->second.buffer.begin() + offset, it->second.buffer.end());
  }
  return true;
}

bool
TcpServer::handleTrafficClient(const struct epoll_event& event) {
  map<int, int>::iterator it = trafficClientMap.find(event.data.fd);
  if (it == trafficClientMap.end()) {
    return false;
  }
  if ((event.events & EPOLLRDHUP) || (event.events & EPOLLERR)) {
    cleanUpTrafficClient(event.data.fd);
    return true;
  }
  if ((event.events & EPOLLIN) == 0) {
    return true;
  }

  char buf[BUFFER_SIZE];
  int len = recv(event.data.fd, buf, BUFFER_SIZE, 0);
  log_debug << "recv, trafficServer <-- tunnelClient <-- *tunnelServer("
    << event.data.fd << ") <-[length=" << len << "]- trafficClient";
  if (len <= 0) {
    cleanUpTrafficClient(event.data.fd);
    return true;
  }
  sendTunnelTraffic(event.data.fd, it->second, event.data.fd, string(buf, len));
  return true;
}

void
TcpServer::run() {
  while(true) {
    struct epoll_event events[MAX_EVENTS];
    int nfds = epoll_wait(epollFd, events, MAX_EVENTS, -1);
    if(nfds == -1) {
      log_error << "failed to epoll_wait";
      exit(EXIT_FAILURE);
    }
    for(int i = 0; i < nfds; i++) {
      if (events[i].data.fd == tunnelServerFd) {
        acceptTunnelClient(tunnelServerFd);
        continue;
      }
      if (trafficServerMap.find(events[i].data.fd) != trafficServerMap.end()) {
        acceptTrafficClient(events[i].data.fd);
        continue;
      }
      handleTunnelClient(events[i]) || handleTrafficClient(events[i]);
    }
  }
}
