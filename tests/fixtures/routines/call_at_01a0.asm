; Calls a routine of its own by address, assembled for $01A0.
    ORG $01A0
    call sub
    ret
sub:
    ld a, $2A
    ret
