//
// Created by Aleksey Timin on 11/18/19.
//

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#elif defined(_WIN32) || defined(WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "utils/Logger.h"
#include "UDPSocket.h"
#include "Platform.h"

namespace eipScanner {
namespace sockets {
	using eipScanner::utils::Logger;
	using eipScanner::utils::LogLevel;

	UDPSocket::UDPSocket(std::string host, int port)
		: UDPSocket(EndPoint(host, port)){

	}

	UDPSocket::UDPSocket(EndPoint endPoint)
			: BaseSocket(EndPoint(std::move(endPoint))) {

		_sockedFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (_sockedFd < 0) {
			throw std::system_error(BaseSocket::getLastError(), BaseSocket::getErrorCategory());
		}

		Logger(LogLevel::DEBUG) << "Opened UDP socket fd=" << _sockedFd;
	}

	UDPSocket::~UDPSocket() {
		Logger(LogLevel::DEBUG) << "Close UDP socket fd=" << _sockedFd;
		Shutdown();
		Close();
	}

	void UDPSocket::Send(const std::vector <uint8_t> &data) const {
		Logger(LogLevel::TRACE) << "Send " << data.size() << " bytes from UDP socket #" << _sockedFd << ".";

		auto addr = _remoteEndPoint.getAddr();
		int count = sendto(_sockedFd, (char*)data.data(), data.size(), MSG_NOSIGNAL,
				(struct sockaddr *)&addr, sizeof(addr));
		if (count < data.size()) {
			throw std::system_error(BaseSocket::getLastError(), BaseSocket::getErrorCategory());
		}
	}

	void UDPSocket::SendTo(const std::vector <uint8_t> &data, const EndPoint& endPoint) const {
		Logger(LogLevel::TRACE) << "Send " << data.size() << " bytes to " << endPoint.toString() << "(UDP socket #" << _sockedFd << ").";

		auto addr = endPoint.getAddr();
		int count = sendto(_sockedFd, (char*)data.data(), data.size(), MSG_NOSIGNAL,
				(struct sockaddr *)&addr, sizeof(addr));
		if (count < data.size()) {
			throw std::system_error(BaseSocket::getLastError(), BaseSocket::getErrorCategory());
		}
	}

	std::vector<uint8_t> UDPSocket::Receive(size_t size) const {
		std::vector<uint8_t> recvBuffer(size);

		auto len = recvfrom(_sockedFd, (char*)recvBuffer.data(), recvBuffer.size(), 0, NULL, NULL);
		Logger(LogLevel::TRACE) << "Received " << len << " bytes from UDP socket #" << _sockedFd << ".";
		if (len < 0) {
			throw std::system_error(BaseSocket::getLastError(), BaseSocket::getErrorCategory());
		}

		return recvBuffer;
	}

	std::vector<uint8_t> UDPSocket::ReceiveFrom(const int socketFd, size_t size, EndPoint& endPoint) {
		std::vector<uint8_t> recvBuffer(size);
		struct sockaddr_in addr;
		socklen_t addrFromLength = sizeof(addr);
		auto len = recvfrom(socketFd, (char*)recvBuffer.data(), recvBuffer.size(), 0, (struct sockaddr*)&addr, &addrFromLength);
		Logger(LogLevel::TRACE) << "Received " << len << " bytes from UDP socket #" << socketFd << ".";
		if (len < 0) {
			throw std::system_error(BaseSocket::getLastError(), BaseSocket::getErrorCategory());
		}

		endPoint = EndPoint(addr);
		return recvBuffer;
	}

	void UDPSocket::select(BaseSocket::SPtr socket, std::chrono::milliseconds timeout, UDPReceiveHandler udpReceiveHandler) {
		auto startTime = std::chrono::steady_clock::now();
		auto stopTime = startTime + timeout;
		int ready;
		do {
			timeval tv = makePortableInterval(std::chrono::duration_cast<std::chrono::milliseconds>(stopTime-startTime));

			fd_set recvSet;
			FD_ZERO(&recvSet);
			FD_SET(socket->getSocketFd(), &recvSet);

			ready = ::select(socket->getSocketFd() + 1, &recvSet, nullptr, nullptr, &tv);
			if (ready < 0) {
				throw std::system_error(BaseSocket::getLastError(), BaseSocket::getErrorCategory());
			}

			if (FD_ISSET(socket->getSocketFd(), &recvSet)) {
				std::vector<uint8_t> data;
				EndPoint endPoint("", 0);
				data = UDPSocket::ReceiveFrom(socket->getSocketFd(), 8192, endPoint);
				fprintf(stderr, "%s: %d,  UDP ReceiveFrom %s (%d size)\n", __FILE__, __LINE__,
					endPoint.toString().c_str(), data.size());

				udpReceiveHandler(data);
			}
			startTime = std::chrono::steady_clock::now();
		} while(ready > 0);
	}

}
}
