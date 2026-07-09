# Install script for directory: /home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/home/jon/research/netchan-v2/demo/nc_rtc/build/dist")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mbedtls" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/aes.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/aria.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/asn1.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/asn1write.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/base64.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/bignum.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/build_info.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/camellia.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ccm.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/chacha20.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/chachapoly.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/check_config.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/cipher.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/cmac.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/compat-2.x.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/config_psa.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/constant_time.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ctr_drbg.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/debug.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/des.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/dhm.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ecdh.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ecdsa.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ecjpake.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ecp.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/entropy.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/error.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/gcm.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/hkdf.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/hmac_drbg.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/legacy_or_psa.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/lms.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/mbedtls_config.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/md.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/md5.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/memory_buffer_alloc.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/net_sockets.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/nist_kw.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/oid.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/pem.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/pk.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/pkcs12.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/pkcs5.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/pkcs7.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/platform.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/platform_time.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/platform_util.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/poly1305.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/private_access.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/psa_util.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ripemd160.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/rsa.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/sha1.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/sha256.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/sha512.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ssl.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ssl_cache.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ssl_ciphersuites.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ssl_cookie.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/ssl_ticket.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/threading.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/timing.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/version.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/x509.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/x509_crl.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/x509_crt.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/mbedtls/x509_csr.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/psa" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_builtin_composites.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_builtin_primitives.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_compat.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_config.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_driver_common.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_driver_contexts_composites.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_driver_contexts_primitives.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_extra.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_platform.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_se_driver.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_sizes.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_struct.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_types.h"
    "/home/jon/research/netchan-v2/demo/nc_rtc/vendor/mbedtls/include/psa/crypto_values.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/jon/research/netchan-v2/demo/nc_rtc/build/mbedtls/include/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
