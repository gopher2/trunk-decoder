# Trunk-Decoder Development Todo

## Plugin Architecture Design
- [x] **Design input/output plugin system architecture for trunk-decoder**
  - Plugin interface for input sources (UDP, shared library, file, etc.)
  - Plugin interface for output destinations (trunk-player, logging, alerts, etc.)
  - Plugin registry and discovery system
  - Configuration management for plugin chains
  - Support 1-to-many input → output connections

- [x] **Implement P25 TSBK UDP input plugin for trunk-recorder data**
  - UDP listener for P25C packets from trunk-recorder
  - P25C packet parsing and validation (magic header, sequencing, checksums)
  - Buffer management and packet reassembly
  - Connection monitoring and reconnection logic
  - Statistics tracking (packets received, dropped, out-of-order)

- [x] **Integrate input plugin into trunk-decoder main application**
  - Plugin lifecycle management (init, start, stop, cleanup)
  - Configuration validation and schema support
  - Input plugin manager with dynamic loading

- [x] **Build and test P25 TSBK data reception from trunk-recorder**
  - End-to-end testing of P25C packet flow
  - Verified plugin loading and initialization
  - Real-time data reception operational

## Plugin Routing and Management
- [ ] **Design flexible plugin routing/wiring system for input-to-output connections**
  - Support configurable 1:1, 1:many, many:1, many:many routing
  - Plugin chain configuration and validation
  - Data flow control and backpressure handling

- [ ] **Implement plugin manager with configurable routing (1:1, 1:many, many:1, many:many)**
  - Enhanced plugin manager with routing capabilities
  - Dynamic reconfiguration support
  - Plugin dependency resolution and ordering
  - Plugin health monitoring and restart capabilities

- [ ] **Refactor trunk-decoder to use plugins for all input/output operations**
  - Convert existing file processing to plugin architecture
  - Migrate all I/O operations through plugin system
  - Remove hardcoded input/output logic

## Input Plugins
- [ ] **Create file input plugin for existing P25 file processing**
  - Support existing P25 file formats for backward compatibility
  - Playback control (speed, loop, seek)
  - Timestamp synchronization and ordering

- [ ] **P25 TSBK shared library input plugin**
  - Shared library interface for direct trunk-recorder integration
  - Memory buffer management and zero-copy optimization
  - Synchronous/asynchronous processing modes
  - Error handling and fallback mechanisms

- [ ] **File input plugin for replay/testing**
  - Support P25 capture file formats for testing
  - Playback control (speed, loop, seek)
  - Timestamp synchronization and ordering

## Output Plugins  
- [ ] **Implement trunk-player API output plugin for web interface**
  - HTTP/WebSocket API integration with trunk-player
  - Real-time event streaming to web interface
  - Authentication and session management
  - Rate limiting and connection management

- [ ] **Create JSON/WAV/text file output plugins**
  - Export P25 events to various formats (JSON, CSV, XML, WAV audio)
  - Log rotation and archival management
  - Filtering and format customization
  - Support existing output format compatibility

- [ ] **Database logging output plugin**
  - Store P25 events in database for historical analysis
  - Configurable retention policies and archiving
  - Query interface for forensic analysis
  - Support multiple database backends (SQLite, PostgreSQL, etc.)

- [ ] **Alert/notification output plugin**
  - Real-time alerting for critical P25 events
  - Multiple notification channels (email, SMS, webhook, etc.)
  - Alert rules engine with filtering and conditions
  - Escalation and acknowledgment handling

## P25 Protocol Engine
- [ ] **Design P25 TSBK packet parsing and protocol analysis engine**
  - Comprehensive P25 TSBK message decoding
  - Protocol state machine and event correlation
  - Reference sdrtrunk implementation for protocol details
  - Support all P25 message types and variants

- [ ] **Implement real-time P25 event detection and alerting system**
  - Emergency activation detection
  - Unit movement and location tracking
  - System configuration change detection
  - Communication pattern analysis

- [ ] **Add comprehensive P25 protocol logging and analysis features**
  - Detailed protocol-level logging
  - Event correlation and timeline reconstruction
  - Communication flow analysis
  - System health and performance monitoring

## Phase 1 Implementation Priority (COMPLETED)
- [x] **Implement P25 TSBK packet receiver** - UDP listener for P25C packets from trunk-recorder  
- [x] **Test end-to-end streaming pipeline** - Verify trunk-recorder → trunk-decoder integration

## Phase 2 Implementation Priority (CURRENT)
- [ ] **Design flexible plugin routing/wiring system** - Configurable 1:1, 1:many, many:1, many:many connections
- [ ] **Implement enhanced plugin manager** - Support all input/output operations via plugins
- [ ] **Create file input plugin** - Backward compatibility with existing P25 file processing
- [ ] **Implement trunk-player API output plugin** - Real-time web interface integration

## Testing and Integration
- [ ] **End-to-end testing framework**
  - Test trunk-recorder → trunk-decoder → trunk-player pipeline
  - Automated testing with P25 test data
  - Performance benchmarking and stress testing
  - Integration testing with live P25 systems

- [ ] **Configuration management and deployment**
  - Plugin configuration schemas and validation
  - Deployment automation and containerization
  - Monitoring and operational tooling
  - Documentation and examples

---
**Current Status**: P25 TSBK input plugin operational, ready for plugin routing system
**Active Input**: P25 TSBK data streaming from trunk-recorder via UDP on port 9999 ✅
**Next Phase**: Design flexible plugin routing for 1:many, many:1, many:many connections
**Target Output**: Real-time event streaming to trunk-player web interface