// CryptoAuth.cc — ECDSA P-256 certificate engine for V2V authentication.
//
// The singleton generates two CA keypairs at startup:
//   Emergency_CA  — signs ambulance/police/priority vehicle certificates
//   Vehicle_CA    — signs normal vehicle certificates
//
// All vehicles call generateKeyPair() + issueCert() during initialize().
// On STATUS_RESPONSE, the receiver calls verifyCert() then verifyProposalSignature()
// before trusting the isPriority flag.

#include "CryptoAuth.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>  // NID_X9_62_prime256v1

// ============================================================
// Singleton
// ============================================================

CryptoAuth& CryptoAuth::instance()
{
    static CryptoAuth inst;   // constructed once, destroyed at program exit
    return inst;
}

// ---- Constructor: spin up both CA keypairs ----
CryptoAuth::CryptoAuth()
{
    emergencyCAKey_ = makeKeyPair(emergencyCAPub_);
    vehicleCAKey_   = makeKeyPair(vehicleCAPub_);
    std::cout << "[CryptoAuth] Emergency_CA and Vehicle_CA key pairs generated." << std::endl;
}

CryptoAuth::~CryptoAuth()
{
    EVP_PKEY_free(emergencyCAKey_);
    EVP_PKEY_free(vehicleCAKey_);
}

// ============================================================
// Public API
// ============================================================

// Generate a fresh EC P-256 keypair for a vehicle.
// Returns the private key (caller must EVP_PKEY_free it).
// Fills pubKeyOut with 65-byte uncompressed public key.
EVP_PKEY* CryptoAuth::generateKeyPair(uint8_t pubKeyOut[CRYPTO_PUBKEY_BYTES])
{
    return makeKeyPair(pubKeyOut);
}

// Issue a signed certificate binding vehiclePubKey to role and issuer.
VehicleCert CryptoAuth::issueCert(const uint8_t vehiclePubKey[CRYPTO_PUBKEY_BYTES],
                                   const std::string& role,
                                   const std::string& issuer)
{
    VehicleCert cert;
    memset(&cert, 0, sizeof(cert));

    memcpy(cert.publicKey, vehiclePubKey, CRYPTO_PUBKEY_BYTES);
    strncpy(cert.role,   role.c_str(),   sizeof(cert.role)   - 1);
    strncpy(cert.issuer, issuer.c_str(), sizeof(cert.issuer) - 1);

    // Choose the signing CA key
    EVP_PKEY* caKey = nullptr;
    if      (issuer == "Emergency_CA") caKey = emergencyCAKey_;
    else if (issuer == "Vehicle_CA")   caKey = vehicleCAKey_;
    else {
        std::cerr << "[CryptoAuth] ERROR: unknown issuer '" << issuer << "'" << std::endl;
        return cert;  // cert has zero-length signature — will fail verification
    }

    // Sign the TBS (to-be-signed) bytes: pubkey || role || issuer
    auto tbs = certTBS(vehiclePubKey, role, issuer);
    bool ok = ecdsaSign(caKey, tbs.data(), tbs.size(), cert.caSignature, cert.caSignatureLen);
    if (!ok) {
        std::cerr << "[CryptoAuth] ERROR: cert signing failed for issuer=" << issuer << std::endl;
    }
    return cert;
}

// Sign (proposalBytes || timestampMs) with the vehicle's own private key.
bool CryptoAuth::signProposal(EVP_PKEY* privKey,
                               const uint8_t* proposalBytes, uint32_t proposalSize,
                               uint64_t timestampMs,
                               uint8_t sigOut[CRYPTO_SIG_MAX_BYTES], uint8_t& sigLen)
{
    // Concatenate proposal bytes and timestamp for signing
    std::vector<uint8_t> tbs(proposalSize + sizeof(uint64_t));
    memcpy(tbs.data(), proposalBytes, proposalSize);
    memcpy(tbs.data() + proposalSize, &timestampMs, sizeof(uint64_t));
    return ecdsaSign(privKey, tbs.data(), tbs.size(), sigOut, sigLen);
}

// Verify the certificate: returns role if valid, "" if forged/unknown issuer.
std::string CryptoAuth::verifyCert(const VehicleCert& cert)
{
    // Determine which CA public key to use based on the claimed issuer
    const uint8_t* caPub = nullptr;
    std::string issuer(cert.issuer);

    if      (issuer == "Emergency_CA") caPub = emergencyCAPub_;
    else if (issuer == "Vehicle_CA")   caPub = vehicleCAPub_;
    else {
        std::cout << "[CryptoAuth] REJECT: unknown issuer '" << issuer << "'" << std::endl;
        return "";
    }

    auto tbs = certTBS(cert.publicKey, std::string(cert.role), issuer);
    bool valid = ecdsaVerify(caPub, tbs.data(), tbs.size(), cert.caSignature, cert.caSignatureLen);

    if (valid) {
        // std::cout << "[CryptoAuth] ACCEPT: cert valid — issuer=" << issuer
        //           << " role=" << cert.role << std::endl;
        return std::string(cert.role);
    } else {
        std::cout << "[CryptoAuth] REJECT: cert signature invalid (fake " << cert.role
                  << " claiming " << issuer << ")" << std::endl;
        return "";
    }
}

// Sign arbitrary bytes — thin public wrapper around ecdsaSign.
// Used by the Quorum Certificate mechanism.
bool CryptoAuth::signBytes(EVP_PKEY* privKey,
                            const uint8_t* data, size_t len,
                            uint8_t sigOut[CRYPTO_SIG_MAX_BYTES], uint8_t& sigLen)
{
    return ecdsaSign(privKey, data, len, sigOut, sigLen);
}

// Verify arbitrary bytes against a raw uncompressed public key.
bool CryptoAuth::verifyBytes(const uint8_t pubKey[CRYPTO_PUBKEY_BYTES],
                              const uint8_t* data, size_t len,
                              const uint8_t* sig, uint8_t sigLen)
{
    return ecdsaVerify(pubKey, data, len, sig, sigLen);
}

// Verify the vehicle's proposal signature using the public key in cert.
bool CryptoAuth::verifyProposalSignature(const VehicleCert& cert,
                                          const uint8_t* proposalBytes, uint32_t proposalSize,
                                          uint64_t timestampMs,
                                          const uint8_t* sig, uint8_t sigLen)
{
    std::vector<uint8_t> tbs(proposalSize + sizeof(uint64_t));
    memcpy(tbs.data(), proposalBytes, proposalSize);
    memcpy(tbs.data() + proposalSize, &timestampMs, sizeof(uint64_t));
    return ecdsaVerify(cert.publicKey, tbs.data(), tbs.size(), sig, sigLen);
}

// ============================================================
// Private helpers
// ============================================================

// Generate an EC P-256 keypair; export uncompressed public key into pubOut.
EVP_PKEY* CryptoAuth::makeKeyPair(uint8_t pubOut[CRYPTO_PUBKEY_BYTES])
{
    // Use EVP_PKEY_CTX for key generation (OpenSSL 1.1+)
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    assert(ctx);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1);

    EVP_PKEY* pkey = nullptr;
    int rc = EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);
    assert(rc == 1 && pkey);

    // Extract EC_KEY to get the uncompressed public key bytes
    EC_KEY* ecKey = EVP_PKEY_get1_EC_KEY(pkey);
    assert(ecKey);
    const EC_POINT* pub = EC_KEY_get0_public_key(ecKey);
    const EC_GROUP* grp = EC_KEY_get0_group(ecKey);

    size_t len = EC_POINT_point2oct(grp, pub, POINT_CONVERSION_UNCOMPRESSED,
                                    pubOut, CRYPTO_PUBKEY_BYTES, nullptr);
    assert(len == CRYPTO_PUBKEY_BYTES);
    EC_KEY_free(ecKey);

    return pkey;  // caller owns this
}

// ECDSA-sign arbitrary data with privKey → DER-encoded sig in sigOut.
bool CryptoAuth::ecdsaSign(EVP_PKEY* key,
                            const uint8_t* data, size_t len,
                            uint8_t* sigOut, uint8_t& sigLen)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    size_t slen = CRYPTO_SIG_MAX_BYTES;

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestSignUpdate(ctx, data, len)                          == 1 &&
        EVP_DigestSignFinal(ctx, sigOut, &slen)                       == 1)
    {
        sigLen = static_cast<uint8_t>(slen);
        ok = true;
    } else {
        std::cerr << "[CryptoAuth] ecdsaSign failed" << std::endl;
    }

    EVP_MD_CTX_free(ctx);
    return ok;
}

// ECDSA-verify: reconstruct EVP_PKEY from raw uncompressed bytes, then verify.
bool CryptoAuth::ecdsaVerify(const uint8_t pubKey[CRYPTO_PUBKEY_BYTES],
                              const uint8_t* data, size_t len,
                              const uint8_t* sig, uint8_t sigLen)
{
    // Reconstruct EC_KEY from raw uncompressed public key bytes
    EC_KEY* ecKey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!ecKey) return false;

    const EC_GROUP* grp = EC_KEY_get0_group(ecKey);
    EC_POINT* point = EC_POINT_new(grp);
    bool ok = false;

    if (EC_POINT_oct2point(grp, point, pubKey, CRYPTO_PUBKEY_BYTES, nullptr) == 1 &&
        EC_KEY_set_public_key(ecKey, point) == 1)
    {
        EVP_PKEY* pkey = EVP_PKEY_new();
        EVP_PKEY_assign_EC_KEY(pkey, ecKey);   // pkey takes ownership of ecKey
        ecKey = nullptr;                        // don't free twice

        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (ctx &&
            EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
            EVP_DigestVerifyUpdate(ctx, data, len)                           == 1 &&
            EVP_DigestVerifyFinal(ctx, sig, sigLen)                          == 1)
        {
            ok = true;
        }
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
    }

    EC_POINT_free(point);
    if (ecKey) EC_KEY_free(ecKey);
    return ok;
}

// Build the byte buffer that CAs sign for a certificate:
//   [ publicKey (65 bytes) | role (16 bytes) | issuer (32 bytes) ]
std::vector<uint8_t> CryptoAuth::certTBS(const uint8_t pubKey[CRYPTO_PUBKEY_BYTES],
                                          const std::string& role,
                                          const std::string& issuer)
{
    std::vector<uint8_t> buf(CRYPTO_PUBKEY_BYTES + 16 + 32, 0);
    memcpy(buf.data(),                       pubKey,        CRYPTO_PUBKEY_BYTES);
    memcpy(buf.data() + CRYPTO_PUBKEY_BYTES, role.c_str(),  std::min(role.size(),   size_t(15)));
    memcpy(buf.data() + CRYPTO_PUBKEY_BYTES + 16, issuer.c_str(), std::min(issuer.size(), size_t(31)));
    return buf;
}
