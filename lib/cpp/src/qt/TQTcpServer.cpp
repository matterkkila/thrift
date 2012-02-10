#include "TQTcpServer.h"

#include <protocol/TProtocol.h>
#include <async/TAsyncProcessor.h>

#include <QTcpSocket>

#include "TQIODeviceTransport.h"

using boost::shared_ptr;
using apache::thrift::protocol::TProtocol;
using apache::thrift::protocol::TProtocolFactory;
using apache::thrift::transport::TTransport;
using apache::thrift::transport::TTransportException;
using apache::thrift::transport::TQIODeviceTransport;
using std::tr1::function;
using std::tr1::bind;

QT_USE_NAMESPACE

namespace apache { namespace thrift { namespace async {

struct TQTcpServer::ConnectionContext {
  shared_ptr<QTcpSocket> connection_;
  shared_ptr<TTransport> transport_;
  shared_ptr<TProtocol> iprot_;
  shared_ptr<TProtocol> oprot_;

  explicit ConnectionContext(shared_ptr<QTcpSocket> connection,
                             shared_ptr<TTransport> transport,
                             shared_ptr<TProtocol> iprot,
                             shared_ptr<TProtocol> oprot)
    : connection_(connection)
    , transport_(transport)
    , iprot_(iprot)
    , oprot_(oprot)
  {}
};

TQTcpServer::TQTcpServer(shared_ptr<QTcpServer> server,
                         shared_ptr<TAsyncProcessor> processor,
                         shared_ptr<TProtocolFactory> pfact,
                         QObject* parent)
  : QObject(parent)
  , server_(server)
  , processor_(processor)
  , pfact_(pfact)
{
  connect(server.get(), SIGNAL(newConnection()), SLOT(processIncoming()));
}

TQTcpServer::~TQTcpServer()
{
}

void TQTcpServer::processIncoming()
{
  while (server_->hasPendingConnections()) {
    // take ownership of the QTcpSocket; technically it could be deleted
    // when the QTcpServer is destroyed, but any real app should delete this
    // class before deleting the QTcpServer that we are using
    shared_ptr<QTcpSocket> connection(server_->nextPendingConnection());
    
    shared_ptr<TTransport> transport;
    shared_ptr<TProtocol> iprot;
    shared_ptr<TProtocol> oprot;
    
    try {
      transport = shared_ptr<TTransport>(new TQIODeviceTransport(connection));
      iprot = shared_ptr<TProtocol>(pfact_->getProtocol(transport));
      oprot = shared_ptr<TProtocol>(pfact_->getProtocol(transport));
    } catch(...) {
      qWarning("[TQTcpServer] Failed to initialize transports/protocols");
      continue;
    }
    
    ctxMap_[connection.get()] =
      shared_ptr<ConnectionContext>(
         new ConnectionContext(connection, transport, iprot, oprot));
    
    connect(connection.get(), SIGNAL(readyRead()), SLOT(beginDecode()));
    
    // need to use QueuedConnection since we will be deleting the socket in the slot
    connect(connection.get(), SIGNAL(disconnected()), SLOT(socketClosed()),
            Qt::QueuedConnection);
  }
}

void TQTcpServer::beginDecode()
{
  QTcpSocket* connection(qobject_cast<QTcpSocket*>(sender()));
  Q_ASSERT(connection);

  if (ctxMap_.find(connection) == ctxMap_.end())
  {
    qWarning("[TQTcpServer] Got data on an unknown QTcpSocket");
    return;
  }
  
  shared_ptr<ConnectionContext> ctx = ctxMap_[connection];
  
  try {
    processor_->process(
      bind(&TQTcpServer::finish, this,
           ctx, std::tr1::placeholders::_1),
      ctx->iprot_, ctx->oprot_);
  } catch(const TTransportException& ex) {
    qWarning("[TQTcpServer] TTransportException during processing: '%s'",
             ex.what());
    ctxMap_.erase(connection);
  } catch(...) {
    qWarning("[TQTcpServer] Unknown processor exception");
    ctxMap_.erase(connection);
  }
}

void TQTcpServer::socketClosed()
{
  QTcpSocket* connection(qobject_cast<QTcpSocket*>(sender()));
  Q_ASSERT(connection);

  if (ctxMap_.find(connection) == ctxMap_.end())
  {
    qWarning("[TQTcpServer] Unknown QTcpSocket closed");
    return;
  }
  
  ctxMap_.erase(connection);
}

void TQTcpServer::finish(shared_ptr<ConnectionContext> ctx, bool healthy)
{
  if (!healthy)
  {
    qWarning("[TQTcpServer] Processor failed to process data successfully");
    ctxMap_.erase(ctx->connection_.get());
  }
}

}}} // apache::thrift::async
