/*
 * cm_cert.h -- DTLS self-signed cert (RSA-2048 / SHA-256, CN=pencg, 4096 days)
 *
 * Generated on host with openssl 3.5.7. Only used for the SRTP-DTLS handshake
 * (peer does not verify: SSL_VERIFY_NONE). The fingerprint must match the
 * a=fingerprint we put in the answer SDP:
 *   sha-256 AF:90:5F:35:76:9A:ED:6F:D1:28:7F:0D:72:94:0B:AC:E0:38:57:99:58:11:EC:93:D6:61:DF:20:49:A5:39:3B
 */
#ifndef CM_CERT_H
#define CM_CERT_H

static const char cm_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDATCCAemgAwIBAgIUZeG0y5a0sgfEoDCO8X/Npieedq0wDQYJKoZIhvcNAQEL\n"
    "BQAwEDEOMAwGA1UEAwwFcGVuY2cwHhcNMjYxMTA2MTYyMzM1WhcNMzYxMTAzMTYy\n"
    "MzM1WjAQMQ4wDAYDVQQDDAVwZW5jZzCCASIwDQYJKoZIhvcNAQEBBQADggEPADCC\n"
    "AQoCggEBAKxHewsyLD8UmXED90JbestFE2+Vck11dqIq+iL4hWgiG5v+eCUBBeOt\n"
    "qiIMnv1jdpL9/9X8RFtrAt+VwyG+Oz9F1D7U8lxFSYTwPfJrG0fSOgDc3oyVBlQJ\n"
    "/CRllvEoE+DNolFszS0niqD6PCOWmu4XQQvYmKATC81eXXXkgQEtfwSUu0LDXbmu\n"
    "wQXZwa8B+t/ZuqqiNCSGPcL6tmjnnsVm3x9HozEVJdCoRsBkOmol/E/olY7NwwKe\n"
    "5hT/OQklaGV6LzCXOOFl0bIHMQAwApCMRZ31Biji3B13ilCkcuGRNIWonl7lsmFx\n"
    "h1ksFgN0zFlpVPYAsVz7Q7TLPLzf7XcCAwEAAaNTMFEwHQYDVR0OBBYEFECqfLex\n"
    "ZnbiH1WG2rFDksYx7cZ7MB8GA1UdIwQYMBaAFECqfLexZnbiH1WG2rFDksYx7cZ7\n"
    "MA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBACd7PHGwvCHsUNyp\n"
    "WoTe5rXRwcJuiI31GKEi3x4RGg8A7d1iXOaf9xz8Hjm2ws7pWO6YbiT/tPScBDJm\n"
    "hTSgpWr7tPHMb6gW2ivrvIAz6Srv+wnAVqG1ZmJn3Cq0fc2gOZa2M5nVdBGrx2on\n"
    "K1RvqFvJYG9KEjlSbtwDWkoGIRHTdg468Y+6ZcpbZkf+BwELgoOyPeYHUmGpV1dv\n"
    "2nltow4dn9H7iuJndkyWqaBpT3HYzjRS0+5ZleXdOKitI6NSCN7s4jHLBPeNwl9G\n"
    "WHIq3c9wDjqw9nkBh3TizKUopqv3xAPprmjZdrC14CpHQA59UkvkvSzn8Oys/m2t\n"
    "ioPEMmo=\n"
    "-----END CERTIFICATE-----\n";

static const char cm_key_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCsR3sLMiw/FJlx\n"
    "A/dCW3rLRRNvlXJNdXaiKvoi+IVoIhub/nglAQXjraoiDJ79Y3aS/f/V/ERbawLf\n"
    "lcMhvjs/RdQ+1PJcRUmE8D3yaxtH0joA3N6MlQZUCfwkZZbxKBPgzaJRbM0tJ4qg\n"
    "+jwjlpruF0EL2JigEwvNXl115IEBLX8ElLtCw125rsEF2cGvAfrf2bqqojQkhj3C\n"
    "+rZo557FZt8fR6MxFSXQqEbAZDpqJfxP6JWOzcMCnuYU/zkJJWhlei8wlzjhZdGy\n"
    "BzEAMAKQjEWd9QYo4twdd4pQpHLhkTSFqJ5e5bJhcYdZLBYDdMxZaVT2ALFc+0O0\n"
    "yzy83+13AgMBAAECggEAEbOukvHu1fR558DukSTxMx2/54FFo2vkarlbxmge3RpU\n"
    "F+1HzxWR01ku2BzP4V1s+6GZvhUTuQj/3JV43Wi8kb3NFz+uP4jnCv/cfaI/xVt2\n"
    "lkwbmfpeEhiMjfjaT6E8mabHfPJfCrq7ZdsQft+kv74n8t2ooRoT5mBB3jFCiCpH\n"
    "GlU7otx0HxNOiHwEUU4Rg7giBfDL5Epoe9svqZDaCE0+kf2BwssBTDD10DudEQxq\n"
    "+nGXpMeARC9+55NuQtwivq1IfxTrcY+FZX3u97F2htkpDfqj+coydbNwIq6jv39Y\n"
    "IcldJTgpz1jOxAR9ph6VXQfomQUfykb7sJmdcIB7sQKBgQDt7D8ucpsi352vMRas\n"
    "q+qPSAbI+woKwL4CIsgtYFCPTlCp5n2GIvFlgwrQJPFyD2UyTnOTDRmmfuDRIJQj\n"
    "5WMl1oZWY7dOQ+Vj4ZW+BSlPwNIRvkH1/cqbdWkmEWcEhlCQfkkBUS/Jhm1oz6Ct\n"
    "4+FZ00d7IxkG0pmocIMTLaiSqwKBgQC5XmxI6bNUy/JKoo9ZivI1nkzVUrph6pNc\n"
    "dioJui50nroZk0PGNxeBru0lDjPazh0D1VY2pokdDwrVMvrdjJeLhK/n1zy7ThZn\n"
    "Y5c4ONLpiu4i4BdZ5zEHj2z7UXVMXWp485/TPgRpXkzNOkV4S4OAEoPezoFywXmi\n"
    "RwujqZowZQKBgCgJHQif2GMh5dLZG8uqkinSIPIo7COrE04gtDaOgn/a2c/9LRop\n"
    "Px2p02P7E3XyCUFR2jjgd3SA3SvTo5n2u5xeYPSgivPskOkhyKTVEYKOG8tIjKjs\n"
    "L+/A7hSt0Pv97giid351PulWaYq9hXg9ZGpNFzYtTcTMI3col6XehQNJAoGBAKo0\n"
    "mRkZ/SAPZBpDyXxeqKezArmdjKX7zfNITLFB0wYh+J8jKUgHrf3VsMolkFeIifvL\n"
    "F652m8+i0D+S3+YuAbiJzTsD1y/lM40bVO66vm7SsnYzbT2PmuVmPJtG4H2OCQWm\n"
    "LUq51zMo8zw+FSuvqr8eQReEZjgIjrcCpR1RzdJBAoGBANUQpYsx1dxBUExhZBXL\n"
    "Q0dLm6YxHWacPbc+g/WXy29kCK8eyzzzppipNJWiiluSyGASh3AHi5JNLlUnVwKn\n"
    "sGtbJd1cVmAQQV03SOnSGyNfwsDjZcOGyZF4s5PLjnhPRKyTfuI7VYhBYuDHTreu\n"
    "qssAsWDi0/MGrkqVRqPOoV1K\n"
    "-----END PRIVATE KEY-----\n";

/* sha-256 fingerprint of our cert, for answer SDP a=fingerprint */
#define CM_CERT_FINGERPRINT \
    "sha-256 AF:90:5F:35:76:9A:ED:6F:D1:28:7F:0D:72:94:0B:AC:E0:38:57:99:58:11:EC:93:D6:61:DF:20:49:A5:39:3B"

#endif /* CM_CERT_H */
