# Common framework interfaces

## Error interface

This library provides the framework's shared error interface:

- `vinc::ErrorCode`: an error code with an `int32_t` underlying type.
- `vinc::Error`: the common failure payload.
- `vinc::Result<T>`: `std::expected<T, Error>`, including `Result<void>`.
- `vinc::error_to_string(const Error&)`: the single diagnostic string entry point.

Link against `vinc::common` and include `<vinc/result.hpp>` or `<vinc/error.hpp>`.
The library depends only on the C++23 standard library.

The error catalog is intentionally empty for now. No code, including zero,
represents success; success is the value state of `Result<T>`. Until named codes
are added, formatting returns `Unknown VINC error (code=<number>)`.
The returned string owns its storage, and formatting may allocate.

Existing component error types have not been migrated. Error codes, additional
diagnostic fields, and adapters will be introduced with those later changes.

## Temporary system settings

`<vinc/config.hpp>` provides the `vinc::config` namespace as a temporary home for
centralized system settings. Settings will be added gradually within explicitly
authorized work and will later be managed by Param.

The header currently contains only an empty namespace and purpose comments.
Existing settings have not been moved. It does not provide dynamic updates,
persistence, or parameter registration.
