#pragma once

#include <string>
#include <system_error>
#include <variant>

namespace lumen {

/// Error codes for the Lumen pipeline.
enum class ErrorCode {
    kOk = 0,
    kNotInitialized,
    kAlreadyInitialized,
    kDeviceCreationFailed,
    kDeviceLost,
    kAccessLost,          // DXGI desktop duplication session lost
    kTimeout,             // Frame acquisition timed out (no desktop change)
    kEncoderError,
    kDecoderError,
    kRendererError,
    kColorConversionError,
    kInvalidArgument,
    kUnsupported,
    kOutOfMemory,
    kAudioCaptureError,
    kAudioEncoderError,
    kAudioDecoderError,
    kAudioRendererError,
    kTransportError,
    kTransportNotConnected,
    kTransportConnectionFailed,
    kTransportTimeout,
    kTransportAddressInvalid,
    kTransportChannelFull,
    kTransportFragmentError,
    kTransportTlsError,
};

/// Lightweight error type carrying a code and optional message.
struct Error {
    ErrorCode code = ErrorCode::kOk;
    std::string message;

    explicit operator bool() const { return code != ErrorCode::kOk; }

    static Error Ok() { return {}; }

    static Error Make(ErrorCode code, std::string msg = {}) {
        return {code, std::move(msg)};
    }
};

/// Result type: holds either a value T or an Error.
template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}

    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    const T& value() const& { return std::get<T>(data_); }
    T& value() & { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }

    const Error& error() const { return std::get<Error>(data_); }

private:
    std::variant<T, Error> data_;
};

/// Specialization for void results.
template <>
class Result<void> {
public:
    Result() : error_(Error::Ok()) {}
    Result(Error error) : error_(std::move(error)) {}

    bool ok() const { return error_.code == ErrorCode::kOk; }
    explicit operator bool() const { return ok(); }

    const Error& error() const { return error_; }

private:
    Error error_;
};

}  // namespace lumen
