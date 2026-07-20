<p align="center">
  <br/>
  <sub><code>MaximumAsp66915/Embeded-RealTime-Systems</code> — branch <code>Distributed-Database-System</code></sub>
</p>

[![View on GitHub](https://img.shields.io/badge/GitHub-View_Repository-blue?logo=GitHub)](https://github.com/MaximumAsp66915/Embeded-RealTime-Systems/tree/Distributed-Database-System)

---

# Project Report — Distributed Database System

This document precedes the design and implementation work carried out
in Phases 1–6. Before any code was written, five background topics were
researched to inform the architectural decisions made throughout the
project — the HTTP library used for the API layers, the landscape of
distributed database systems, the general concept of an API, the local
network the cluster runs on, and the properties of distributed systems
in general. Findings are summarized briefly below; per-phase design
decisions, diagrams, and evaluation are documented in each phase's own
`Report.md`, indexed at the end of this document.

## Research Items

### 1. The Mongoose Library in C/C++

[Mongoose](https://mongoose.ws) is a lightweight, embeddable networking
library written in C (usable directly from C++) that ships as a single
`mongoose.c`/`mongoose.h` pair with no external dependencies, making it
well suited to embedded and resource-constrained environments as well
as ordinary servers. It provides an event-driven core supporting TCP,
UDP, and TLS, on top of which it implements HTTP/HTTPS, WebSocket, and
MQTT, along with utilities for serving static files, handling file
uploads, and parsing JSON-RPC-style payloads. To implement an HTTP
server or REST API in C/C++, an application defines a single event
handler function, binds it to a listening address with
`mg_http_listen()`, and dispatches incoming `MG_EV_HTTP_MSG` events by
matching the request URI/method (e.g. `mg_http_match_uri()`) to the
appropriate handler, which reads query parameters or a JSON body and
writes a response with `mg_http_reply()`. Because the whole server runs
inside a single poll loop (`mg_mgr_poll()`), it can be embedded directly
inside a larger application — this is exactly how it is used in this
project, both as a standalone gateway process and as a background
thread inside `master_node` — without needing a separate process or a
heavyweight framework like Apache or Nginx for the application layer
itself.

### 2. Distributed Databases

A distributed database spreads data storage and query processing across
multiple networked nodes rather than a single machine, in order to
scale beyond one machine's capacity, tolerate individual node failures,
and keep data close to where it is produced or consumed. Several
well-known systems illustrate different points in this design space:
**Apache Cassandra**, a wide-column store built for linear horizontal
scalability and high write throughput with no single point of failure,
commonly used for time-series and event-logging workloads; **MongoDB**,
a document-oriented database that shards collections across a cluster
and is widely used for flexible-schema application data; **Apache
HBase**, a column-family store built on top of HDFS, used for very
large-scale analytical and log-processing workloads; **CockroachDB**,
a distributed SQL database that provides strong (serializable)
consistency and automatic range-based sharding while still exposing a
standard relational/SQL interface; and **etcd**, a small distributed
key-value store built on the Raft consensus algorithm, used less for
application data and more for cluster configuration and coordination
(e.g. in Kubernetes). This project's own cluster — one Master and two
Slaves, each with an independent local SQLite database that the Master
queries or cascades to — is a deliberately simplified, educational
instance of the same underlying idea: partitioning data across nodes
and coordinating lookups between them.

### 3. API

An Application Programming Interface (API) is a defined contract that
lets one piece of software request functionality or data from another
without needing to know its internal implementation — only the
interface it exposes. In practice this typically means a set of
callable functions, endpoints, or messages, each with a specified input
and output format. APIs are the standard mechanism by which independent
services and applications communicate: a mobile app calling a backend
over HTTP, a backend calling a third-party payment provider, or, as in
this project, an operator or monitoring tool calling into `master_node`
over HTTP, MQTT, or SNMP to retrieve sensor data without knowing
anything about the SQLite schema or the cascade logic behind it. Well
designed APIs decouple the caller from the callee's implementation
details, so either side can change internally as long as the contract
between them stays stable — which is precisely what allowed this
project to add MQTT, SNMP, and a logging API to the same underlying
data store across separate phases without altering how the data itself
was stored.

### 4. Local Area Network (LAN)

A Local Area Network is a network that connects computers and devices
within a limited physical area — a building, lab, or, in this project's
case, a set of virtual machines on the same virtualized network segment
— as opposed to a Wide Area Network, which spans much larger
geographic distances. Nodes on a LAN are typically connected through
switches (and historically hubs) and communicate using Ethernet at the
link layer and IP at the network layer, with each node assigned an
address on the same subnet so that any node can reach any other
directly, without traversing a router to the public internet. This is
exactly the arrangement the cluster in this project relies on: the
Master and both Slave VMs sit on the same private subnet
(`192.168.56.0/24` in the reference setup), each bound to its own fixed
IP, which is what allows the Master to dial each Slave's local port
directly (or through NGINX's port-based proxying, in Phase 1's advanced
configuration) with predictable, low-latency connectivity — a property
that would not hold, and would need to be re-designed around, if the
nodes were spread across a WAN instead.

### 5. Distributed Systems

A distributed system is a collection of independent, networked
components that must coordinate and communicate to appear, to the
outside world, as a single coherent system. Their defining
characteristics include concurrency (multiple nodes operate at once),
the absence of a single global clock, and independent, partial failure
— any individual node or link can fail without necessarily bringing
down the whole system. These properties give distributed systems real
advantages: horizontal scalability well beyond what one machine can
provide, fault tolerance through redundancy (if one node is
unreachable, the system can still function using the others), and the
ability to place data or processing close to where it is needed. They
also introduce well-known challenges: maintaining consistency between
copies of the same data across nodes, handling network partitions and
partial failures gracefully, coordinating clocks and ordering events
without a shared clock, and managing the added complexity of testing
and debugging behavior that only appears when multiple nodes interact.
This project's own cluster surfaces a version of nearly every one of
these challenges at a small, manageable scale: the Master's cascade
logic has to tolerate a Slave being unreachable, the caching layer in
Phase 2 has to reason about staleness between the cache and the
underlying database, and the move from HTTP to MQTT in Phase 3
reflects a real trade-off between simple request/response coupling and
a more loosely coupled, asynchronous communication model.

## Phase reports index

Each phase's own `Report.md` documents that phase's design decisions,
diagrams, and evaluation in full — this index is kept in sync as each
phase's report is updated:

- [Phase 1 Report — Basic Design of a Distributed Database System](Phase1/Report.md)
- [Phase 2 Report — Caching and Two-Layer Database Structure](Phase2/Report.md)
- [Phase 3 Report — System Connection to the MQTT Protocol](Phase3/Report.md)
- [Phase 4 Report — Reading Sensor Information using the SNMP Protocol](Phase4/Report.md)
- [Phase 5 Report — API Development for Reading Sensor Logs](Phase5/Report.md)
- [Phase 6 Report — Design and Implementation of the Alert System](Phase6/Report.md)

For the overall project layout, the simplified system diagram, and
cross-cutting implementation notes, see [`README.md`](README.md).
