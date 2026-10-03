; The average of A and X, in A — a 65816 routine with no ORG of its own, placed where it fits. The carry
; the add leaves rotates back in, so the halving is exact for any two bytes.
        A8
        X8
        STA $00
        TXA
        CLC
        ADC $00
        ROR A
        RTS
