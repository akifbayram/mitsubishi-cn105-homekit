# Companion to sl2_sodium_slim.c — include AFTER idf_component_register().
# Routes libsodium's internal calls to its per-primitive-family
# implementation selectors through the stubs in sl2_sodium_slim.c, so unused
# families are never pulled from the archive. Byte-identical across the
# firmwares that share sl2_sodium_slim.c; keep the list in sync with the
# __wrap_ stubs at the bottom of the .c file.
foreach(sym
        _crypto_pwhash_argon2_pick_best_implementation
        _crypto_scalarmult_curve25519_pick_best_implementation
        _crypto_generichash_blake2b_pick_best_implementation
        _crypto_onetimeauth_poly1305_pick_best_implementation
        _crypto_stream_chacha20_pick_best_implementation
        _crypto_stream_salsa20_pick_best_implementation
        _crypto_aead_aegis128l_pick_best_implementation
        _crypto_aead_aegis256_pick_best_implementation)
    # -u forces the stub object out of libmain.a: ld does not extract archive
    # members to satisfy __wrap_ references on its own.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${sym}" "-Wl,-u,__wrap_${sym}")
endforeach()
