# Add Rust Bindings for NORM

## Summary

This PR adds complete Rust bindings for the NORM library, enabling developers to use NORM's reliable multicast capabilities within the Rust language.

## What's Added

### Core Implementation
- **norm-sys**: Low-level FFI bindings auto-generated via bindgen from `normApi.h`
- **norm**: Rust wrappers with RAII resource management
- **Build Integration**: Seamless integration with NORM's waf build system

### API Coverage
- ✅ Instance management with automatic cleanup
- ✅ Session configuration (sender/receiver modes)
- ✅ Data, file, and stream object transfers
- ✅ Complete stream operations (write, read, flush, mark EOM, seek)
- ✅ Event handling via Rust iterators
- ✅ Ergonomic multicast configuration with builder pattern and macro
- ✅ Type-safe enums for all NORM constants (30+ event types, 9 enum types)
- ✅ Comprehensive error handling with `thiserror`

### Documentation
- Complete `README.md` with installation and usage examples
- Detailed `API_GUIDE.md` covering all major components
- Inline rustdoc documentation for all public APIs
- 6 working examples demonstrating data, file, and stream transfers

### Key Features
- **Memory Safety**: RAII pattern ensures proper resource cleanup via Drop trait
- **Zero-Cost Abstractions**: Compiles to same machine code as direct C calls
- **Idiomatic Rust**: Builder patterns, iterators, Result types, method chaining
- **Platform Support**: macOS, Linux, Windows, Solaris (via conditional compilation)

## Building with Rust Bindings

```bash
# Configure with Rust support
./waf configure --build-rust

# Build in release mode with documentation
./waf configure --build-rust --rust-release --rust-docs
./waf build

# Or build directly with cargo
cd rust
cargo build --release
cargo doc --open
```

## Usage Example

```rust
use norm::{Instance, multicast, MulticastExt, EventType, Result};

fn main() -> Result<()> {
    let instance = Instance::new(false)?;
    let session = instance.create_session("224.1.2.3", 6003, 1)?;

    // Ergonomic multicast configuration
    let config = multicast!("224.1.2.3", 6003, {
        ttl: 64,
        loopback: true,
    });
    session.with_multicast(&config)?;

    // Start sender and send data
    session.set_tx_rate(1_000_000.0)
           .start_sender(rand::random(), 1024*1024, 1400, 64, 16, None)?;

    session.data_enqueue(b"Hello, NORM!", None)?;

    // Event handling with iterators
    for event in instance.events() {
        if event.event_type == EventType::TxQueueEmpty {
            break;
        }
    }

    Ok(())
}
```

## Examples Included

- `data_send.rs` / `data_recv.rs` - Data object transfer
- `file_send.rs` / `file_recv.rs` - File transfer
- `stream_send.rs` / `stream_recv.rs` - Stream transfer with message boundaries

Run examples:
```bash
cargo run --example data_send 224.1.2.3 6003
cargo run --example stream_send 224.1.2.3 6003
```

## Testing

The bindings include comprehensive unit tests covering:
- Type conversions (all enum From implementations)
- Multicast configuration builder pattern
- Multicast address validation (IPv4 and IPv6)
- Error handling and display
- String conversion functions

```bash
# Run tests (set library path for dynamic linking)
cd src/rust
DYLD_LIBRARY_PATH=../../build:../../build/protolib cargo test

# Or use the waf build which handles paths automatically
./waf build --build-rust
```

**Test Results:**
- ✅ 17 unit tests in `norm` crate (all passing)
- ✅ 2 unit tests in `norm-sys` crate (all passing)
- ✅ 6 working examples compile and demonstrate functionality

Test coverage includes:
- `error.rs`: 5 tests (error handling, string conversion)
- `types.rs`: 5 tests (enum conversions)
- `multicast.rs`: 7 tests (builder, validation, macro)

## Files Added

```
rust/
├── norm-sys/           # FFI bindings (bindgen)
│   ├── build.rs       # Auto-generates bindings from normApi.h
│   ├── Cargo.toml
│   └── src/lib.rs
├── norm/              # Safe Rust wrappers
│   ├── Cargo.toml
│   ├── build.rs
│   └── src/
│       ├── lib.rs        # Public API
│       ├── error.rs      # Error types
│       ├── types.rs      # Type-safe enums
│       ├── instance.rs   # Instance wrapper
│       ├── session.rs    # Session wrapper
│       ├── object.rs     # Object wrapper + stream ops
│       ├── node.rs       # Node wrapper
│       ├── event.rs      # Event handling
│       └── multicast.rs  # Multicast config
├── examples/          # 6 complete examples
├── Cargo.toml        # Workspace configuration
├── README.md         # Rust documentation
├── API_GUIDE.md      # Detailed API guide
└── waf_rust.py       # Build system integration
```

## Technical Details

- **Language**: Rust 2021 edition, requires Rust 1.70+
- **FFI**: Uses bindgen 0.69 for automatic binding generation
- **Dependencies**: `libc`, `thiserror`, `rand` (examples only)
- **Build**: Integrates with waf via custom Python module
- **Linking**: Automatically links against libnorm, libprotokit, and system libraries
- **Safety**: All unsafe FFI calls wrapped in safe Rust APIs

## Compatibility

- Non-breaking change - purely additive
- Does not modify existing C/C++ code
- Optional - NORM builds without Rust if toolchain not present
- Requires Rust toolchain only when `--build-rust` flag is used

## License

Rust bindings follow NORM's BSD-3-Clause license.

---

## Checklist

- [x] Code compiles without errors
- [x] Examples demonstrate all major features
- [x] Documentation is comprehensive
- [x] Unit tests cover core functionality (19 tests passing)
- [x] Integration with waf build system
- [x] Platform-specific configuration (macOS, Linux, Windows, Solaris)
- [x] RAII resource management prevents leaks
- [x] Error handling throughout
- [x] Stream operations fully implemented
- [x] FFI constant naming warnings suppressed appropriately
- [x] Workspace resolver configured for edition 2021
