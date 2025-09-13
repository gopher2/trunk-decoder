# trunk-decoder

A high-performance P25 digital radio decoder that processes raw `.p25` bitstream files, converting them into decoded audio (WAV) and metadata (JSON) files.

**trunk-decoder serves as a processing bridge between trunk-recorder and trunk-player**, enabling distributed P25 decoding workflows. This allows offloading computationally intensive processing such as IMBE voice decoding and AI transcription to separate systems while significantly reducing the traffic between recording and compute endpoints. 

## Features

### Core Capabilities
- **Real IMBE Voice Decoding**: Converts raw P25 frames into actual decoded speech audio
- **Storage Efficiency**: P25 files consume considerably less storage space than WAV files
- **Batch Processing**: Process single files or entire directories of `.p25` files
- **Standard Output Formats**: Generates WAV and JSON files compatible with trunk-player
- **Protocol Analysis**: Detailed P25 frame analysis including DUID, NAC, and frame structure that can output to various formats
- **Encrypted Call Support**: Processes encrypted calls for metadata extraction and audio decoding

### Workflow Integration
- **trunk-recorder → trunk-decoder → trunk-player**: Complete processing pipeline
- **Resource Distribution**: CPU-intensive decoding on separate hardware from capture systems
- **Space Optimization**: Store compact P25 files, decode on-demand or in batches

## Installation

### Build

```bash
git clone <repository-url>
cd trunk-decoder
mkdir build
cd build
cmake ..
make -j
```

### Command Line Options

| Option | Description |
|--------|-------------|
| `-h, --help` | Show help message |
| `-c, --config FILE` | Use JSON config file for all settings |
| `-i, --input PATH` | Input P25 file or directory |
| `-o, --output DIR` | Output directory (default: current directory) |
| `-v, --verbose` | Enable verbose output with detailed frame information |
| `-q, --quiet` | Quiet mode (minimal output) |
| `-r, --recursive` | Process subdirectories recursively |
| `-k, --key KEYID:KEY` | Add decryption key (hex format, auto-detects algorithm) |

### Output Format Options (Must specify at least one)

| Option | Description |
|--------|-------------|
| `--json` | Generate JSON metadata files |
| `--wav` | Generate WAV audio files |
| `--text` | Generate text dump files with frame analysis |
| `--csv` | Generate CSV frame analysis files for spreadsheet analysis |

### Additional Format Options

| Option | Description |
|--------|-------------|
| `--aac` | Generate AAC audio files (unimplemented) |
| `--mp3` | Generate MP3 audio files (unimplemented) |
| `--transcript` | Generate voice transcription (unimplemented) |

## Output Files

For each input file `filename.p25`, trunk-decoder can generate (based on command-line options):

- **`filename.wav`** - 16-bit, 8kHz mono WAV audio file with decoded speech (--wav)
- **`filename.json`** - Call metadata in JSON format with transmission details (--json)  
- **`filename.txt`** - Text dump with detailed P25 frame analysis (--text)
- **`filename.csv`** - CSV frame data for spreadsheet analysis with encryption details (--csv)

### JSON Metadata Format

```json
{
  "freq": 856187500.0,
  "start_time": 1757705318,
  "stop_time": 1757705325,
  "call_length": 6.66,
  "talkgroup": 22102,
  "audio_type": "digital",
  "emergency": 0,
  "encrypted": 0,
  "decoder_source": "trunk-decoder",
  "input_file": "/captures/22102-1757705318_856187500.0-call_4.p25",
  "p25_frames": 37,
  "voice_frames": 37,
  "error_count": 0,
  "spike_count": 0,
  "srcList": [
    {
      "src": 2009817,
      "time": 1757705318,
      "pos": 0.0,
      "emergency": 0
    }
  ],
  "freqList": [
    {
      "freq": 856187500.0,
      "time": 1757705318,
      "pos": 0.0,
      "len": 6.66,
      "error_count": 0,
      "spike_count": 0
    }
  ]
}
```

## Technical Details

### P25 Frame Support

- **LDU1 (0x05)**: Logical Data Unit 1 with voice and link control data
- **LDU2 (0x0A)**: Logical Data Unit 2 with voice and encryption sync data
- **IMBE Decoding**: Full implementation using trunk-recorder's proven IMBE vocoder

### Audio Processing

- Each P25 voice frame contains 9 IMBE codewords
- Each IMBE codeword produces 160 audio samples (20ms at 8kHz)
- Total: 1440 samples per P25 frame (180ms of audio)
- Output: 16-bit signed PCM at 8kHz sample rate

### Performance

- Typical processing speed: 100-500x real-time
- Memory efficient: Processes files individually
- Parallel processing: can handle multiple files simultaneously

### Encryption Support

trunk-decoder supports P25 voice decryption with multiple algorithms:

- **DES-OFB**: Data Encryption Standard in Output Feedback mode (Algorithm ID 0x81)
- **AES-256**: Advanced Encryption Standard 256-bit in OFB mode (Algorithm ID 0x84)
- **ADP/RC4**: Advanced Digital Privacy using RC4 stream cipher (Algorithm ID 0xAA)

#### Key Management

Keys can be specified via command line with automatic algorithm detection:

```bash
# DES-OFB key (8 bytes)
./trunk-decoder --wav --json -k 001:0102030405060708 input.p25

# ADP/RC4 key (5 bytes)  
./trunk-decoder --wav --json -k 002:0102030405 input.p25

# AES-256 key (32 bytes)
./trunk-decoder --wav --json -k 003:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20 input.p25

# Multiple keys for different talkgroups
./trunk-decoder --wav --json -k 001:0102030405060708 -k 002:0102030405 input.p25
```

**Algorithm Detection**: Key length automatically determines encryption algorithm:
- 5 bytes = ADP/RC4
- 8 bytes = DES-OFB  
- 32 bytes = AES-256

#### Configuration File Keys

JSON config files can specify decryption keys (implementation pending):

```json
{
  "decryption_keys": [
    {
      "keyid": "0x001",
      "key": "0102030405060708",
      "description": "DES-OFB example key (8 bytes)"
    },
    {
      "keyid": "0x002", 
      "key": "0102030405",
      "description": "ADP/RC4 example key (5 bytes)"
    }
  ]
}
```

## Input File Format

### Distributed Workflow Architecture

trunk-decoder enables a distributed P25 processing architecture:

```
┌─────────────────┐    ┌─────────────────────┐      ┌─────────────────┐
│  trunk-recorder │───▶│  trunk-decoder      │─────▶│  trunk-player   │
│                 │.p25│                     │.mp3  │                 │
│ • RF Capture    │    │ • IMBE Decoding     │.json │ • Web Interface │
│ • P25 Framing   │    │ • Batch Processing  │      │ • Audio Playback│
│ • Metadata      │    │ • Protocol Analysis │      │ • Call Browsing │
└─────────────────┘    └─────────────────────┘      └─────────────────┘
     Capture System        Processing Node           Playback System
```

### Storage & Processing Benefits

- **Compact Storage**: P25 files consume significantly less space than equivalent WAV files
- **Processing Offload**: CPU-intensive IMBE decoding moved to dedicated systems
- **Batch Processing**: Process large volumes of P25 files efficiently
- **Resource Scaling**: Multiple decoder instances can process parallel queues

### P25 File Format

trunk-decoder processes `.p25` files containing raw P25 digital radio frames:

- **File Structure**: Binary files with consecutive P25 frames
- **Frame Format**: Each frame includes DUID, NAC, length, and payload data
- **Supported Types**: LDU1 (0x05) and LDU2 (0x0A) voice frames
- **File Naming**: Compatible with trunk-recorder: `talkgroup-timestamp_frequency-call_X.p25`

### P25 Frame Structure

Each P25 frame in the input file follows this format:
```
[DUID (1 byte)] [NAC (2 bytes)] [Length (2 bytes)] [Frame Data (variable)]
```

- **DUID**: Data Unit Identifier (0x05 for LDU1, 0x0A for LDU2)
- **NAC**: Network Access Code (system identifier)
- **Length**: Frame payload length in bytes
- **Frame Data**: Raw P25 frame bits containing IMBE voice data

## Configuration File

trunk-decoder supports JSON configuration files for batch processing and service deployment.

**Sample Configurations**: Use the provided config files as starting points:
- `config.example.json` - Basic configuration with decryption keys
- `config.service.json` - API service mode configuration

### Basic Config File Format

```json
{
  "input_path": "/path/to/p25/files/",
  "output_dir": "/path/to/output/",
  "recursive": true,
  
  "enable_json": true,
  "enable_wav": true,
  "enable_text": false,
  
  "verbose": false,
  "quiet": false,
  "process_encrypted": true,
  "skip_empty_frames": false
}
```

### Service Mode Config (API Integration)

```json
{
  "service_mode": "api",
  "api_endpoint": "http://localhost:3000/api/v1/decode",
  
  "enable_json": true,
  "enable_wav": true,
  "enable_text": true,
  
  "output_dir": "/tmp/trunk-decoder/",
  "audio_format": "wav",
  "audio_sample_rate": 8000,
  "verbose": true,
  "process_encrypted": true
}
```

#### Running as API Service

Start trunk-decoder in API service mode:

```bash
./trunk-decoder -c config-api-service.json
```

The service will start on the configured port (default: 3000) and listen for decode requests.

#### API Endpoints

**POST /api/v1/decode**
- Accepts multipart/form-data with:
  - `p25_file`: Binary P25 file data
  - `metadata`: JSON call metadata (optional)
- Returns JSON response with processed files and statistics

**GET /api/v1/status**
- Returns service status and version information

#### API Security

The trunk-decoder API supports authentication and HTTPS/TLS encryption for secure deployments.

##### Authentication

Authentication is enabled by setting an auth token in the service configuration:

```json
{
  "service_mode": "api",
  "api_port": 3000,
  "auth_token": "your-secret-token-here",
  "output_dir": "/tmp/trunk-decoder/"
}
```

Clients can authenticate using either:

1. **Bearer Token** (recommended):
```bash
curl -X POST https://localhost:3000/api/v1/decode \
  -H "Authorization: Bearer your-secret-token-here" \
  -F "p25_file=@call_123.p25"
```

2. **X-API-Key Header**:
```bash
curl -X POST https://localhost:3000/api/v1/decode \
  -H "X-API-Key: your-secret-token-here" \
  -F "p25_file=@call_123.p25"
```

##### HTTPS/TLS Encryption

Enable HTTPS by providing SSL certificate and key files:

```json
{
  "service_mode": "api",
  "api_port": 8443,
  "auth_token": "your-secret-token-here",
  "ssl_cert": "/path/to/certificate.pem",
  "ssl_key": "/path/to/private_key.pem",
  "output_dir": "/tmp/trunk-decoder/"
}
```

**Self-Signed Certificate Example:**
```bash
# Generate private key
openssl genrsa -out trunk-decoder.key 2048

# Generate self-signed certificate (valid for 365 days)
openssl req -new -x509 -key trunk-decoder.key -out trunk-decoder.crt -days 365 \
  -subj "/C=US/ST=State/L=City/O=Organization/CN=localhost"

# Convert to PEM format (if needed)
cat trunk-decoder.crt trunk-decoder.key > trunk-decoder.pem
```

**Security Considerations:**
- Use strong, randomly generated auth tokens (32+ characters)
- Store certificates and keys in secure locations with appropriate file permissions (600)
- Consider using a reverse proxy (nginx, Apache) for production deployments
- Rotate auth tokens and certificates regularly
- Use proper CA-signed certificates for production environments

#### trunk-recorder Plugin Configuration

Configure the trunk-decoder plugin in trunk-recorder's configuration:

```json
{
  "plugins": [
    {
      "name": "trunk_decoder",
      "library": "libtrunkrec_plugin_trunk_decoder.so",
      "enabled": true,
      "server": "https://decoder.example.com:8443/api/v1/decode",
      "auth_token": "your-secret-token-here",
      "verify_ssl": true,
      "timeout": 30,
      "systems": [
        {
          "short_name": "system1",
          "enabled": true,
          "server": "https://system1-decoder.example.com:8443/api/v1/decode",
          "auth_token": "system1-specific-token",
          "verify_ssl": true,
          "compress_data": false
        }
      ]
    }
  ]
}
```

**Configuration Options:**
- `server`: Full URL to trunk-decoder API endpoint (include https:// for encrypted connections)
- `auth_token`: Authentication token for API access
- `verify_ssl`: Enable/disable SSL certificate verification (default: true)
- `timeout`: Request timeout in seconds (default: 30)
- `enabled`: Enable/disable the plugin globally or per-system
- `compress_data`: Enable HTTP compression for data transfer (default: false)
- System-specific settings override global defaults

**Note:** The plugin only sends P25 files and metadata to trunk-decoder for processing. All decoding, decryption, and output generation happens on the trunk-decoder side. The plugin does not write any output files locally.

#### API Service Behavior

When running in API service mode, trunk-decoder:
- Processes P25 files submitted via HTTP POST requests
- Returns processing results and statistics in JSON format  
- Currently writes temporary output files to the configured output directory
- Returns file paths in the API response for client access

**Future Enhancement:** For production deployments, the API could be enhanced to return processed audio data directly in the HTTP response rather than writing files to disk.

#### Example API Usage

```bash
# Using curl to send a P25 file for processing
curl -X POST http://localhost:3000/api/v1/decode \
  -F "p25_file=@call_123.p25" \
  -F 'metadata={"talkgroup":123,"freq":856187500}'
```

Response:
```json
{
  "success": true,
  "files": {
    "wav_file": "/tmp/trunk-decoder-output/api_call_1757722345.wav",
    "json_file": "/tmp/trunk-decoder-output/api_call_1757722345.json"
  },
  "stats": {
    "frames_processed": "23",
    "voice_frames": "23", 
    "audio_samples": "33120",
    "duration_seconds": "4.14"
  }
}
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `input_path` | string | - | Input file or directory path |
| `output_dir` | string | "." | Output directory |
| `recursive` | boolean | false | Process subdirectories |
| `enable_json` | boolean | false | Generate JSON metadata files |
| `enable_wav` | boolean | false | Generate WAV audio files |
| `enable_text` | boolean | false | Generate text analysis files |
| `verbose` | boolean | false | Enable verbose output |
| `quiet` | boolean | false | Quiet mode |
| `process_encrypted` | boolean | true | Process encrypted calls |
| `skip_empty_frames` | boolean | false | Skip frames without voice data |
| `service_mode` | string | "file" | Processing mode ("file" or "api") |
| `api_endpoint` | string | - | API endpoint for service mode |
| `audio_format` | string | "wav" | Audio output format |
| `audio_sample_rate` | integer | 8000 | Audio sample rate (Hz) |

### Complete Configuration Template

Here's a comprehensive configuration template with all available options:

```json
{
  "input_path": "/path/to/p25/files/",
  "output_dir": "/path/to/output/",
  "recursive": true,
  "enable_json": true,
  "enable_wav": true,
  "enable_text": false,
  "verbose": false,
  "quiet": false,
  "process_encrypted": true,
  "skip_empty_frames": false,
  "include_frame_analysis": true,
  "service_mode": "file",
  "api_endpoint": "http://localhost:3000/api/v1/decode",
  "audio_format": "wav",
  "decryption_keys": [
    {
      "keyid": "0x001",
      "key": "0102030405060708",
      "description": "DES-OFB key (8 bytes)"
    },
    {
      "keyid": "0x002",
      "key": "0102030405",
      "description": "ADP/RC4 key (5 bytes)"  
    },
    {
      "keyid": "0x003",
      "key": "0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20",
      "description": "AES-256 key (32 bytes)"
    }
  ]
}
```

**Key Length Determines Algorithm:**
- 5 bytes = ADP/RC4 encryption
- 8 bytes = DES-OFB encryption  
- 32 bytes = AES-256 encryption

**Configuration Scenarios:**

**Batch Processing:**
- Set `"service_mode": "file"`
- Configure `input_path` and `output_dir`
- Enable desired output formats (`enable_json`, `enable_wav`, etc.)

**API Service:**
- Set `"service_mode": "api"`
- Configure `api_endpoint` URL
- Set `output_dir` for temporary files

## Author

**David Kierzkowski (K9DPD)** - Initial development and implementation

## License

This project is licensed under the **GNU General Public License v3.0** - see the [LICENSE](LICENSE) file for details.

### License Terms Summary

**GPL v3 ensures this software remains open source:**

- **Free Use**: Use for any purpose (personal, commercial, research)
- **Modify & Distribute**: Make changes and share them
- **Patent Protection**: Contributors grant patent rights
- **No Proprietary Forks**: All derivatives must remain GPL v3
- **No Closed Source**: Modified versions must include source code

**If you distribute modified versions of this software, you MUST:**
- Provide complete source code to recipients
- License your modifications under GPL v3
- Include attribution to original authors
- Document your changes clearly

This **copyleft license** ensures that all improvements benefit the open source community.

### Component Licenses

This project incorporates components from:
- **OP25**: GNU General Public License v3
- **IMBE Vocoder**: Pavel Yazev implementation (GPL compatible)

See individual source files for specific license information.

## Acknowledgments

This project incorporates substantial code and architecture from:

- **trunk-recorder project**: Core P25 processing, IMBE vocoder integration, and frame handling logic - the foundation that made this decoder possible
- **OP25 project**: P25 protocol implementation and IMBE frame parsing
- **Boatboad OP25**: P25 encryption implementations (DES, AES-256, ADP/RC4) - comprehensive P25 decryption support from the Boatboad OP25 fork
- **Pavel Yazev**: IMBE encoder/decoder fixed-point implementation
- **GNU Radio community**: Underlying DSP framework concepts
- **P25 standards community**: Digital radio protocol specifications

**Special thanks to the trunk-recorder maintainers and contributors** - this project builds directly on their excellent P25 implementation work.

**Special thanks to Boatboad** for the comprehensive P25 encryption implementations that enable full decryption support across DES, AES-256, and ADP/RC4 algorithms.
