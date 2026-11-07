; Calls a routine of its own by address, assembled for wherever the machine places it.
    call sub
    ret
sub:
    ld a, $2A
    ret
