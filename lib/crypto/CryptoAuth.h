#pragma once
// CryptoAuth.h — Lightweight ECDSA (P-256) certificate authority for V2V authentication.
//
// Design:
//   - CryptoAuth is a singleton that holds two CA keypairs (Emergency_CA, Vehicle_CA).
//   - Each vehicle generates its own EC keypair at init time.
//   - Ambulance/Police/Priority vehicles get a cert signed by Emergency_CA; normal vehicles by Vehicle_CA.
//   - On receive, the cert is verified against the trusted CA public keys.
//   - isPriority is set ONLY if the cert is signed by Emergency_CA (never from payload).
//
// Requires: OpenSSL (link with -lssl -lcrypto)

#include <cstdint>
#include <string>
#include <vector>
#include <openssl/evp.h>

// Fixed byte sizes (P-256 uncompressed pubkey = 65, max DER ECDSA sig = 72)
static constexpr int CRYPTO_PUBKEY_BYTES  = 65;
static constexpr int CRYPTO_SIG_MAX_BYTES = 72;

// ---- Certificate: issued by a CA, binds a public key to a role ----
// Plain C struct so it can be memcpy'd into a network payload.
struct VehicleCert {
    uint8_t publicKey[CRYPTO_PUBKEY_BYTES];   // vehicle's EC public key (uncompressed)
    char    role[16];                          // "ambulance" or "normal"
    char    issuer[32];                        // "Emergency_CA" or "Vehicle_CA"
    uint8_t caSignature[CRYPTO_SIG_MAX_BYTES]; // CA's ECDSA signature over (pubkey+role+issuer)
    uint8_t caSignatureLen;                    // actual bytes used in caSignature[]
};

// ---- Signed STATUS_RESPONSE payload ----
// Sent instead of a raw VehicleProposal so the receiver can verify authenticity.
// proposalBytes holds sizeof(VehicleProposal) raw bytes; proposalSize records the count.
struct SignedProposal {
    uint8_t    proposalBytes[256]; // serialized VehicleProposal (padded for safety)
    uint32_t   proposalSize;       // exact bytes written
    uint64_t   timestampMs;        // simtime in ms, prevents replay between runs
    VehicleCert cert;              // vehicle's cert (must be verified before trusting role)
    uint8_t    signature[CRYPTO_SIG_MAX_BYTES]; // vehicle's ECDSA sig over proposalBytes+timestamp
    uint8_t    signatureLen;
};

// ---- Singleton crypto engine ----
class CryptoAuth {
public:
    // Returns the process-wide singleton (created on first call).
    static CryptoAuth& instance();

    // Called once per vehicle at initialize():
    //   - Generates an EC P-256 keypair for this vehicle.
    //   - Fills pubKeyOut[CRYPTO_PUBKEY_BYTES] with the uncompressed public key.
    //   - Returns a heap-allocated EVP_PKEY* (caller must EVP_PKEY_free() at finish()).
    EVP_PKEY* generateKeyPair(uint8_t pubKeyOut[CRYPTO_PUBKEY_BYTES]);

    // Issues a VehicleCert signed by the named CA.
    //   issuer must be "Emergency_CA" or "Vehicle_CA".
    //   Aborts (EV_fatal) if issuer is unknown.
    VehicleCert issueCert(const uint8_t vehiclePubKey[CRYPTO_PUBKEY_BYTES],
                          const std::string& role,
                          const std::string& issuer);

    // Signs (proposalBytes || timestampMs) with the vehicle's own private key.
    //   Returns true on success; fills sigOut and sigLen.
    bool signProposal(EVP_PKEY* privKey,
                      const uint8_t* proposalBytes, uint32_t proposalSize,
                      uint64_t timestampMs,
                      uint8_t sigOut[CRYPTO_SIG_MAX_BYTES], uint8_t& sigLen);

    // Verifies the certificate against the trusted CA public keys.
    //   Returns the role string ("ambulance" / "normal") if valid, "" if forged/unknown.
    //   NEVER trust role from the network payload — always call this.
    std::string verifyCert(const VehicleCert& cert);

    // Verifies the vehicle's message signature using the public key embedded in cert.
    //   Call after verifyCert() — if the cert is fake, this check is meaningless.
    bool verifyProposalSignature(const VehicleCert& cert,
                                 const uint8_t* proposalBytes, uint32_t proposalSize,
                                 uint64_t timestampMs,
                                 const uint8_t* sig, uint8_t sigLen);

    // General-purpose sign/verify for arbitrary byte buffers.
    // Used by the Quorum Certificate mechanism to sign (round || schedule).
    bool signBytes(EVP_PKEY* privKey,
                   const uint8_t* data, size_t len,
                   uint8_t sigOut[CRYPTO_SIG_MAX_BYTES], uint8_t& sigLen);
    bool verifyBytes(const uint8_t pubKey[CRYPTO_PUBKEY_BYTES],
                     const uint8_t* data, size_t len,
                     const uint8_t* sig, uint8_t sigLen);

    // Non-copyable singleton
    CryptoAuth(const CryptoAuth&)            = delete;
    CryptoAuth& operator=(const CryptoAuth&) = delete;

private:
    CryptoAuth();   // generates both CA keypairs
    ~CryptoAuth();  // frees CA keys

    EVP_PKEY* emergencyCAKey_;                      // Emergency_CA private key
    EVP_PKEY* vehicleCAKey_;                        // Vehicle_CA private key
    uint8_t   emergencyCAPub_[CRYPTO_PUBKEY_BYTES]; // Emergency_CA public key (for verification)
    uint8_t   vehicleCAPub_[CRYPTO_PUBKEY_BYTES];   // Vehicle_CA public key  (for verification)

    // Internal helpers
    EVP_PKEY* makeKeyPair(uint8_t pubOut[CRYPTO_PUBKEY_BYTES]);
    bool      ecdsaSign(EVP_PKEY* key, const uint8_t* data, size_t len,
                        uint8_t* sigOut, uint8_t& sigLen);
    bool      ecdsaVerify(const uint8_t pubKey[CRYPTO_PUBKEY_BYTES],
                          const uint8_t* data, size_t len,
                          const uint8_t* sig, uint8_t sigLen);
    // Builds the byte buffer that the CA signs: pubkey || role || issuer
    static std::vector<uint8_t> certTBS(const uint8_t pubKey[CRYPTO_PUBKEY_BYTES],
                                        const std::string& role,
                                        const std::string& issuer);
};
