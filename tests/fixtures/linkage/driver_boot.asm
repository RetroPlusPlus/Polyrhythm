; Driver-image scan probe: an `.asm` image with NO `.policy` on its DriverImagePath, so it exercises the
; unset-resolves-to-Embed rule. Assembles to a recognizable byte string the test asserts.
ld a, $5B
ret
