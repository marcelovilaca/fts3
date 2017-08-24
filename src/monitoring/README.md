Monitoring (fts_msg_bulk)
=========================

This subsystem consumes messages persisted on disk coming from FTS3
events, and routes them depending on their type.

Reads from a DirQ (queue on the filesystem), where each message
is a serialized json object prefixed with its type:

* **ST** Transfer *ST*art
* **CO** Transfer *CO*mpletion
* **SS** Transfer *S*tatu*S* change
* **OP** *OP*timizer

As they are read (by MsgInbound), they are routed internally
using a [*Publish Subcribe*](http://www.enterpriseintegrationpatterns.com/patterns/messaging/PublishSubscribeChannel.html)
pattern, built with ZeroMQ.

On the other side we have a consumer (MsgOutboundExternal)
implementing a [*Content Based Router*](http://www.enterpriseintegrationpatterns.com/patterns/messaging/ContentBasedRouter.html)
combined with a [*Messaging Bridge*](http://www.enterpriseintegrationpatterns.com/patterns/messaging/MessagingBridge.html),
so each message type is directed to the appropriate topic of
an ActiveMQ broker.
