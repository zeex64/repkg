#pragma once

#include <cstddef>
#include <span>

namespace sunrise::middleware::crypto::aes_gcm {

/** Package and channel material is AES-128. */
inline constexpr std::size_t kKeySize = 16;
/** Both callers use the standard 12-byte nonce. */
inline constexpr std::size_t kNonceSize = 12;
/** Both callers use the full 16-byte tag. */
inline constexpr std::size_t kTagSize = 16;

/**
 * Authenticates and decrypts one buffer whose tag is stored apart from its ciphertext.
 * @param key AES-128 key.
 * @param nonce 12-byte nonce.
 * @param tag Expected authentication tag.
 * @param output Destination of at least the ciphertext size.
 * @return True only when the transform succeeds and the tag authenticates.
 */
[[nodiscard]] bool decrypt(std::span<const std::byte, kKeySize> key,
                           std::span<const std::byte, kNonceSize> nonce,
                           std::span<const std::byte> ciphertext,
                           std::span<const std::byte, kTagSize> tag,
                           std::span<std::byte> output) noexcept;

/**
 * Encrypts and authenticates one buffer with a detached full-width GCM tag.
 * @param key AES-128 key.
 * @param nonce 12-byte package nonce.
 * @param plaintext Source bytes.
 * @param output Destination of at least the plaintext size.
 * @param tag Receives the 16-byte authentication tag.
 * @return True only when every byte and the detached tag are produced.
 */
[[nodiscard]] bool encrypt(std::span<const std::byte, kKeySize> key,
                           std::span<const std::byte, kNonceSize> nonce,
                           std::span<const std::byte> plaintext,
                           std::span<std::byte> output,
                           std::span<std::byte, kTagSize> tag) noexcept;

} // namespace sunrise::middleware::crypto::aes_gcm
