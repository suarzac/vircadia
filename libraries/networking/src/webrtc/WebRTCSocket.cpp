//
//  WebRTCSocket.cpp
//  libraries/networking/src/webrtc
//
//  Created by David Rowe on 21 Jun 2021.
//  Copyright 2021 Vircadia contributors.
//

#include "WebRTCSocket.h"

#if defined(WEBRTC_DATA_CHANNELS)

#include "../NetworkLogging.h"
#include "../udt/Constants.h"


WebRTCSocket::WebRTCSocket(QObject* parent, NodeType_t nodeType) :
    QObject(parent),
    _signalingServer(this /*, QHostAddress::AnyIPv4, DEFAULT_DOMAIN_SERVER_WS_PORT*/),
    _dataChannels(this, nodeType)
{
    // Connect WebRTC signaling server and data channels.
    connect(&_signalingServer, &WebRTCSignalingServer::messageReceived,
        &_dataChannels, &WebRTCDataChannels::onSignalingMessage);
    connect(&_dataChannels, &WebRTCDataChannels::signalingMessage,
        &_signalingServer, &WebRTCSignalingServer::sendMessage);

    // Route received data channel messages.
    connect(&_dataChannels, &WebRTCDataChannels::dataMessage, this, &WebRTCSocket::onDataChannelReceivedMessage);
}

void WebRTCSocket::setSocketOption(QAbstractSocket::SocketOption option, const QVariant& value) {
    clearError();
    switch (option) {
    case QAbstractSocket::SocketOption::ReceiveBufferSizeSocketOption:
    case QAbstractSocket::SocketOption::SendBufferSizeSocketOption:
        // WebRTC doesn't provide access to setting these buffer sizes.
        break;
    default:
        setError(QAbstractSocket::SocketError::UnsupportedSocketOperationError, "Failed to set socket option");
        qCCritical(networking_webrtc) << "WebRTCSocket::setSocketOption() not implemented for option:" << option;
    }

}

QVariant WebRTCSocket::socketOption(QAbstractSocket::SocketOption option) {
    clearError();
    switch (option) {
    case QAbstractSocket::SocketOption::ReceiveBufferSizeSocketOption:
        // WebRTC doesn't provide access to the receive buffer size. Just use the default buffer size.
        return udt::WEBRTC_RECEIVE_BUFFER_SIZE_BYTES;
    case QAbstractSocket::SocketOption::SendBufferSizeSocketOption:
        // WebRTC doesn't provide access to the send buffer size though it's probably 16MB. Just use the default buffer size.
        return udt::WEBRTC_SEND_BUFFER_SIZE_BYTES;
    default:
        setError(QAbstractSocket::SocketError::UnsupportedSocketOperationError, "Failed to get socket option");
        qCCritical(networking_webrtc) << "WebRTCSocket::getSocketOption() not implemented for option:" << option;
    }

    return QVariant();
}

bool WebRTCSocket::bind(const QHostAddress& address, quint16 port, QAbstractSocket::BindMode mode) {
    // WebRTC data channels aren't bound to ports so just treat this as a successful operation.
    auto wasBound = _isBound;
    _isBound = _signalingServer.bind(address, port);
    if (_isBound != wasBound) {
        emit stateChanged(_isBound ? QAbstractSocket::BoundState : QAbstractSocket::UnconnectedState);
    }
    return _isBound;
}

QAbstractSocket::SocketState WebRTCSocket::state() const {
    return _isBound ? QAbstractSocket::BoundState : QAbstractSocket::UnconnectedState;
}

void WebRTCSocket::abort() {
    _dataChannels.reset();
}


qint64 WebRTCSocket::writeDatagram(const QByteArray& datagram, quint16 port) {
    clearError();
    if (_dataChannels.sendDataMessage(port, datagram)) {
        return datagram.length();
    }
    setError(QAbstractSocket::SocketError::UnknownSocketError, "Failed to write datagram");
    return -1;
}

qint64 WebRTCSocket::bytesToWrite(quint16 port) const {
    return _dataChannels.getBufferedAmount(port);
}


bool WebRTCSocket::hasPendingDatagrams() const {
    return _receivedQueue.length() > 0;
}

qint64 WebRTCSocket::pendingDatagramSize() const {
    if (_receivedQueue.length() > 0) {
        return _receivedQueue.head().second.length();
    }
    return -1;
}

qint64 WebRTCSocket::readDatagram(char* data, qint64 maxSize, QHostAddress* address, quint16* port) {
    clearError();
    if (_receivedQueue.length() > 0) {
        auto datagram = _receivedQueue.dequeue();
        auto length = std::min((qint64)datagram.second.length(), maxSize);

        if (data) {
            memcpy(data, datagram.second.constData(), length);
        }

        if (address) {
            // WEBRTC TODO: Use signaling channel's remote WebSocket address? Or remote data channel address?
            *address = QHostAddress::AnyIPv4;
        }

        if (port) {
            *port = datagram.first;
        }

        return length;
    }
    setError(QAbstractSocket::SocketError::UnknownSocketError, "Failed to read datagram");
    return -1;
}


QAbstractSocket::SocketError WebRTCSocket::error() const {
    return _lastErrorType;
}

QString WebRTCSocket::errorString() const {
    return _lastErrorString;
}


void WebRTCSocket::setError(QAbstractSocket::SocketError errorType, QString errorString) {
    _lastErrorType = errorType;
}

void WebRTCSocket::clearError() {
    _lastErrorType = QAbstractSocket::SocketError();
    _lastErrorString = QString();
}


void WebRTCSocket::onDataChannelReceivedMessage(int dataChannelID, const QByteArray& message) {
    _receivedQueue.enqueue(QPair<int, QByteArray>(dataChannelID, message));
    emit readyRead();
}

#endif // WEBRTC_DATA_CHANNELS
