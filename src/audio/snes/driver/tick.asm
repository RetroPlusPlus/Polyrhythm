; The default sound driver's tick, called once a frame. It turns the two mailboxes into voice commands,
; hands every voice's waiting command to the sound program over the ports, plays each voice's mode out —
; a voice playing once is keyed off at the end of its sample, a repeating one struck again when its
; period runs out — and copies what the program reports into the bytes the game reads back. The work RAM
; it reads is laid out in init.asm.
;
; A voice's block asks for a key-on (1), a key-off (2) or a change to the voice as it plays (3) — its
; volume and pitch, with no key-on. A command on the ports is its arguments in ports 1 to 3, then the
; command byte in port 0: the command in bits 7-5 ($20 volume, $40 key-on, $60 key-off, $80 pitch), the
; toggle in bit 4, the voice in bits 2-0. The sound
; program echoes the byte on port 0 when it has taken the command, and the tick waits for that before the
; next one; the toggle flips after each, so two identical commands are both seen.

        ORG $00:8400
        A8
        X8
tick:   STZ $65                 ; no voice keyed on yet this frame
        LDA $61
        BEQ quick
        STZ $61
        LDX #$00
alloff: LDA #$02
        STA $20,X               ; every voice: key-off
        STZ $26,X               ; and its mode over
        TXA
        CLC
        ADC #$08
        TAX
        CPX #$40
        BNE alloff
quick:  LDA $60
        CMP #$FF
        BEQ scan
        STA $21                 ; voice 0: the sample
        LDA #$FF
        STA $60                 ; taken
        STZ $22                 ; pitch $1000, the sample as recorded
        LDA #$10
        STA $23
        LDA #$7F
        STA $24                 ; full, both sides
        STA $25
        STZ $26                 ; continuous
        LDA #$01
        STA $20                 ; key-on
scan:   LDX #$00
next:   LDA $20,X
        BEQ skip
        CMP #$02
        BEQ off
        PHA                     ; 1 key-on or 3 change: both begin with the volume
        LDA $24,X
        STA !$2141              ; port 1: the left volume
        LDA $25,X
        STA !$2142              ; port 2: the right volume
        JSR !voice
        ORA #$20                ; volume
        JSR !send
        PLA
        CMP #$03
        BEQ change
        JSR !keyon
        BRA done
change: LDA $22,X
        STA !$2142              ; port 2: the pitch, low
        LDA $23,X
        STA !$2143              ; port 3: the pitch, high
        JSR !voice
        ORA #$80                ; pitch, on the voice as it plays
        JSR !send
        BRA done
off:    STZ $26,X               ; the mode is over with the voice
        JSR !voice
        ORA #$60                ; key-off
        JSR !send
done:   STZ $20,X               ; taken
skip:   TXA
        CLC
        ADC #$08
        TAX
        CPX #$40
        BNE next

        LDA !$2141              ; ENDX, as the sound program reports it
        STA $63

; The modes. A voice keyed on this frame is left alone: the chip clears its ENDX bit at the key-on, and
; the bit the program last reported is from before it.
        LDX #$00
modes:  LDA $26,X
        BEQ nomode
        JSR !voice
        PHX
        TAX
        LDA !bits,X
        PLX
        STA $66                 ; the voice's bit
        AND $65
        BNE nomode              ; keyed on this frame
        LDA $26,X
        CMP #$02
        BNE ended
        PHX                     ; a repeat: count the frames down to its next strike
        TXA
        LSR A
        LSR A
        TAX                     ; the voice's word, voice x 2
        REP #$20
        A16
        LDA $80,X
        DEC A
        STA $80,X
        BNE waiting
        LDA $70,X
        STA $80,X               ; the period again
        SEP #$20
        A8
        PLX
        JSR !keyon              ; struck again
        BRA nomode
waiting: SEP #$20
        A8
        PLX
ended:  LDA $63
        AND $66
        BEQ nomode              ; still playing
        JSR !voice
        ORA #$60                ; key-off: the pass is over
        JSR !send
        LDA $26,X
        CMP #$01
        BNE nomode
        STZ $26,X               ; once: done
nomode: TXA
        CLC
        ADC #$08
        TAX
        CPX #$40
        BNE modes
        INC $64
        RTS

; Key the voice whose block X points at on: its sample and pitch to the ports, the key-on command, and
; the voice's bit in the frame's keyed set.
keyon:  LDA $21,X
        STA !$2141              ; port 1: the sample's directory entry
        LDA $22,X
        STA !$2142              ; port 2: the pitch, low
        LDA $23,X
        STA !$2143              ; port 3: the pitch, high
        JSR !voice
        PHX
        TAX
        LDA !bits,X
        PLX
        ORA $65
        STA $65
        JSR !voice
        ORA #$40                ; key-on
        JMP !send

bits:   DB $01,$02,$04,$08,$10,$20,$40,$80
        A8                      ; the widths again, past the data
        X8

; The voice whose block X points at, in A.
voice:  TXA
        LSR A
        LSR A
        LSR A
        RTS

; Send the command in A — its arguments already in ports 1 to 3 — with the toggle, wait for the sound
; program to echo it, and flip the toggle for the next one.
send:   ORA $62
        STA !$2140
echo:   CMP !$2140
        BNE echo
        LDA $62
        EOR #$10
        STA $62
        RTS
