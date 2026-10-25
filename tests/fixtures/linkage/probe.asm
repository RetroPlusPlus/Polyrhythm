; Routine-registry linkage probe: assembles to a short, recognizable byte string so the test can assert
; the build's baked bytecode reached the program rather than merely that some span is non-empty.
ld a, $2A
ret
