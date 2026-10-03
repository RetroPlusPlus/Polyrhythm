; The average of A and X, in A. No ORG: the routine lands where the machine has room for it. The carry
; the add leaves rotates back in, so the halving is exact for any two bytes.
        A8
        X8
        STA $00
        TXA
        CLC
        ADC $00
        ROR A
        RTS
