//
// Created by Aleksey Timin on 11/18/19.
//
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <random>

#include "ConnectionManager.h"
#include "eip/CommonPacket.h"
#include "cip/connectionManager/ForwardOpenRequest.h"
#include "cip/connectionManager/ForwardCloseRequest.h"
#include "cip/connectionManager/LargeForwardOpenRequest.h"
#include "cip/connectionManager/ForwardOpenResponse.h"
#include "cip/connectionManager/NetworkConnectionParams.h"
#include "cip/connectionManager/NetworkConnectionParametersBuilder.h"
#include "utils/Logger.h"
#include "utils/Buffer.h"

namespace eipScanner {
	using namespace cip::connectionManager;
	using namespace utils;
	using cip::MessageRouterResponse;
	using cip::EPath;
	using cip::GeneralStatusCodes;
	using eip::CommonPacket;
	using sockets::UDPSocket;
	using sockets::UDPBoundSocket;
	using sockets::BaseSocket;

	enum class ConnectionManagerServiceCodes : cip::CipUsint {
		FORWARD_OPEN = 0x54,
		LARGE_FORWARD_OPEN = 0x5B,
		FORWARD_CLOSE = 0x4E
	};

	sockets::UDPBoundSocket::SPtr ConnectionManager::_udpServerSocket{};
	std::map<cip::CipUint, IOConnection::SPtr> ConnectionManager::_connectionMap{};

	ConnectionManager::ConnectionManager()
		: ConnectionManager(std::make_shared<MessageRouter>()){
	}

	ConnectionManager::ConnectionManager(const MessageRouter::SPtr& messageRouter)
		: _messageRouter(messageRouter){
		std::random_device rd;
		std::uniform_int_distribution<cip::CipUint> dist(0, std::numeric_limits<cip::CipUint>::max());
		_incarnationId = dist(rd);
	}

	ConnectionManager::~ConnectionManager() = default;

	IOConnection::WPtr
	ConnectionManager::forwardOpen(const SessionInfoIf::SPtr& si, ConnectionParameters connectionParameters, bool isLarge) {
		static int serialNumberCount = 0;
		connectionParameters.connectionSerialNumber = ++serialNumberCount;

		NetworkConnectionParametersBuilder o2tNCP(connectionParameters.o2tNetworkConnectionParams, isLarge);
		NetworkConnectionParametersBuilder t2oNCP(connectionParameters.t2oNetworkConnectionParams, isLarge);

		if (o2tNCP.getConnectionType() == NetworkConnectionParametersBuilder::MULTICAST) {
			static cip::CipUint idCount = _incarnationId << 16;
			connectionParameters.o2tNetworkConnectionId = ++idCount;
		}

		if (t2oNCP.getConnectionType() == NetworkConnectionParametersBuilder::P2P) {
			static cip::CipUdint idCount = _incarnationId << 16;
			connectionParameters.t2oNetworkConnectionId = ++idCount;
		}

		auto o2tSize = o2tNCP.getConnectionSize();
		auto t2oSize = t2oNCP.getConnectionSize();

		connectionParameters.connectionPathSize = (connectionParameters.connectionPath .size() / 2)
				+ (connectionParameters.connectionPath.size() % 2);

		if ((connectionParameters.transportTypeTrigger & NetworkConnectionParams::CLASS1) > 0
			|| (connectionParameters.transportTypeTrigger & NetworkConnectionParams::CLASS3) > 0) {
			connectionParameters.o2tNetworkConnectionParams += 2;
			connectionParameters.t2oNetworkConnectionParams += 2;
		}

		if (connectionParameters.o2tRealTimeFormat) {
			connectionParameters.o2tNetworkConnectionParams += 4;
		}

		if (connectionParameters.t2oRealTimeFormat) {
			connectionParameters.t2oNetworkConnectionParams += 4;
		}

		MessageRouterResponse messageRouterResponse;
		if (isLarge) {
			LargeForwardOpenRequest request(connectionParameters);
			messageRouterResponse = _messageRouter->sendRequest(si,
				static_cast<cip::CipUsint>(ConnectionManagerServiceCodes::LARGE_FORWARD_OPEN),
				EPath(6, 1), request.pack(), {});
		} else {
			ForwardOpenRequest request(connectionParameters);
			messageRouterResponse = _messageRouter->sendRequest(si,
				static_cast<cip::CipUsint>(ConnectionManagerServiceCodes::FORWARD_OPEN),
				EPath(6, 1), request.pack(), {});
		}

		IOConnection::SPtr ioConnection;
		if (messageRouterResponse.getGeneralStatusCode() == GeneralStatusCodes::SUCCESS) {
			ForwardOpenResponse response;
			response.expand(messageRouterResponse.getData());

			Logger(LogLevel::INFO) << "Open IO connection O2T_ID=" << std::hex << response.getO2TNetworkConnectionId()
				<< " T2O_ID=" << std::hex << response.getT2ONetworkConnectionId()
				<< " SerialNumber " << std::hex << response.getConnectionSerialNumber();

			ioConnection.reset(new IOConnection());
			ioConnection->_o2tNetworkConnectionId = response.getO2TNetworkConnectionId();
			ioConnection->_t2oNetworkConnectionId = response.getT2ONetworkConnectionId();
			ioConnection->_o2tAPI = response.getO2TApi();
			ioConnection->_t2oAPI = response.getT2OApi();
			ioConnection->_connectionTimeoutMultiplier = 4 << connectionParameters.connectionTimeoutMultiplier;
			ioConnection->_serialNumber = response.getConnectionSerialNumber();
			ioConnection->_transportTypeTrigger = connectionParameters.transportTypeTrigger;
			ioConnection->_o2tRealTimeFormat = connectionParameters.o2tRealTimeFormat;
			ioConnection->_t2oRealTimeFormat = connectionParameters.t2oRealTimeFormat;
			ioConnection->_connectionPath = connectionParameters.connectionPath;
			ioConnection->_originatorVendorId = connectionParameters.originatorVendorId;
			ioConnection->_originatorSerialNumber = connectionParameters.originatorSerialNumber;

			ioConnection->_o2tDataSize = o2tSize;
			ioConnection->_t2oDataSize = t2oSize;
			ioConnection->_o2tFixedSize = (o2tNCP.getType() == NetworkConnectionParametersBuilder::FIXED);
			ioConnection->_t2oFixedSize = (t2oNCP.getType() == NetworkConnectionParametersBuilder::FIXED);

			const eip::CommonPacketItem::Vec &additionalItems = messageRouterResponse.getAdditionalPacketItems();
			auto o2tSockAddrInfo = std::find_if(additionalItems.begin(), additionalItems.end(),
					[](auto item) { return item.getTypeId() == eip::CommonPacketItemIds::O2T_SOCKADDR_INFO; });

			if (o2tSockAddrInfo != additionalItems.end()) {
				Buffer sockAddrBuffer(o2tSockAddrInfo->getData());
				sockets::EndPoint endPoint("",0);
				sockAddrBuffer >> endPoint;

				if (endPoint.getHost() == "0.0.0.0") {
//					ioConnection->_serverSocket = std::make_unique<UDPSocket>(
//							si->getRemoteEndPoint().getHost(), endPoint.getPort());
				    auto socket = findOrCreateSocket(sockets::EndPoint(si->getRemoteEndPoint().getHost(), endPoint.getPort()));
				    ioConnection->_serverSocket = std::make_unique<UDPSocket>(*socket.get());
				    fprintf(stderr, "%s: %d,  ********************************************\n", __FILE__, __LINE__);
				} else {
					ioConnection->_serverSocket = std::make_unique<UDPSocket>(endPoint);
				}
				// findOrCreateSocket(sockets::EndPoint(si->getRemoteEndPoint().getHost(), EIP_DEFAULT_IMPLICIT_PORT));
				sockets::EndPoint remoteEndPoint(si->getRemoteEndPoint().getHost(), EIP_DEFAULT_IMPLICIT_PORT);
				findOrCreateSocket(remoteEndPoint);
				ioConnection->_remoteEndPoint = remoteEndPoint;
			} else {
				sockets::EndPoint remoteEndPoint(si->getRemoteEndPoint().getHost(), EIP_DEFAULT_IMPLICIT_PORT);
				auto socket = findOrCreateSocket(remoteEndPoint);
				ioConnection->_serverSocket = std::make_unique<UDPSocket>(*socket.get());
				ioConnection->_remoteEndPoint = remoteEndPoint;
//			    auto socket = findOrCreateSocket(sockets::EndPoint(si->getRemoteEndPoint().getHost(), EIP_DEFAULT_IMPLICIT_PORT));
//			    ioConnection->_serverSocket = std::make_unique<UDPSocket>(*socket.get());
//				// ioConnection->_socket = std::make_unique<UDPSocket>(si->getRemoteEndPoint().getHost(), EIP_DEFAULT_IMPLICIT_PORT);
			}

			Logger(LogLevel::INFO) << "Open UDP socket to send data to "
					<< ioConnection->_serverSocket->getRemoteEndPoint().toString();

//			findOrCreateSocket(sockets::EndPoint(si->getRemoteEndPoint().getHost(), EIP_DEFAULT_IMPLICIT_PORT));

			auto result = _connectionMap
					.insert(std::make_pair(response.getT2ONetworkConnectionId(), ioConnection));
			if (!result.second) {
				Logger(LogLevel::ERROR)
					<< "Connection with T2O_ID=" << std::hex << response.getT2ONetworkConnectionId()
					<< " already exists.";
			}
		} else {
			logGeneralAndAdditionalStatus(messageRouterResponse);
		}

		return ioConnection;
	}

	IOConnection::WPtr
	ConnectionManager::largeForwardOpen(const SessionInfoIf::SPtr& si, ConnectionParameters connectionParameters) {
		return forwardOpen(si, connectionParameters, true);
	}

	void ConnectionManager::forwardClose(const SessionInfoIf::SPtr& si, const IOConnection::WPtr& ioConnection) {
		if (auto ptr = ioConnection.lock()) {
			ForwardCloseRequest request;

			request.setConnectionPath(ptr->_connectionPath);
			request.setConnectionSerialNumber(ptr->_serialNumber);
			request.setOriginatorVendorId(ptr->_originatorVendorId);
			request.setOriginatorSerialNumber(ptr->_originatorSerialNumber);

			Logger(LogLevel::INFO) << "Close connection T2O_ID=" << std::hex
					<< ptr->_t2oNetworkConnectionId;

			auto messageRouterResponse = _messageRouter->sendRequest(si,
					static_cast<cip::CipUsint>(ConnectionManagerServiceCodes::FORWARD_CLOSE),
					EPath(6, 1), request.pack());

			if (messageRouterResponse.getGeneralStatusCode() != GeneralStatusCodes::SUCCESS) {
				Logger(LogLevel::WARNING)
					<< "Failed to close the connection in target with error=0x"
					<< std::hex
					<< messageRouterResponse.getGeneralStatusCode()
					<< ". But the connection is removed from ConnectionManager anyway";
			}

			auto rc = _connectionMap.erase(ptr->_t2oNetworkConnectionId);
			(void) rc;
			assert(rc);
		} else {
			Logger(LogLevel::WARNING) << "Attempt to close an already closed connection";
		}
	}

	void ConnectionManager::handleConnections(std::chrono::milliseconds timeout) {
		UDPSocket::select(_udpServerSocket, timeout, [this](std::vector<uint8_t> recvData){
			CommonPacket commonPacket;
			commonPacket.expand(recvData);
			fprintf(stderr, "%s: %d, notifyReceiveData size: %d (socket fd=%d)\n", __FILE__, __LINE__, recvData.size(), _udpServerSocket->getSocketFd());
			// TODO: Check TypeIDs and sequence of the packages
			Buffer buffer(commonPacket.getItems().at(0).getData());
			cip::CipUdint connectionId;
			buffer >> connectionId;
			Logger(LogLevel::DEBUG) << "Received data from connection T2O_ID=" << std::hex << connectionId;
			auto io = _connectionMap.find(connectionId);
			if (io != _connectionMap.end()) {
				io->second->notifyReceiveData(commonPacket.getItems().at(1).getData());
				Logger(LogLevel::ERROR) << "Received data from connection T2O_ID=" << std::hex
							<< connectionId;
			} else {
				for (auto x : _connectionMap) {
					fprintf(stderr, "%s: %d,  0x%x\n", __FILE__, __LINE__, x.first);
				}
				Logger(LogLevel::ERROR) << "Received data from unknown connection T2O_ID=" << std::hex
							<< connectionId;
			}
		});

		std::vector<cip::CipUdint> connectionsToClose;
		for (auto& entry : _connectionMap) {
			if (!entry.second->notifyTick()) {
				connectionsToClose.push_back(entry.first);
			}
		}

		for (auto& id : connectionsToClose) {
			_connectionMap.erase(id);
		}
	}

	UDPBoundSocket::SPtr ConnectionManager::findOrCreateSocket(const sockets::EndPoint& endPoint) {

		if (!_udpServerSocket.get()) {
		    _udpServerSocket = std::make_shared<UDPBoundSocket>("", EIP_DEFAULT_IMPLICIT_PORT);
		}

		return _udpServerSocket;
	}

	bool ConnectionManager::hasOpenConnections() const {
		return !_connectionMap.empty();
	}
}
