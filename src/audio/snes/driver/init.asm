; The default sound driver's init, run once when the driver is hosted: it clears the driver's bytes in work
; RAM and sends the sound program to the audio unit through the upload protocol the unit's boot program
; waits in.
;
; The boot program waits with $AA and $BB in ports 0 and 1. The init names the destination, $0200, in
; ports 2 and 3, asks for a transfer with a non-zero port 1, and kicks with $CC in port 0; then it sends
; each byte in port 1 with its index in port 0, waiting for the unit to echo the index back. After the last
; byte it names $0200 again, asks for a start with a zero port 1, and kicks with the last index plus two.
; Then it waits for the sound program's own word that it is set up and polling — $00 on port 0 — so the
; first command the tick sends finds it listening.
;
; The sound program is one page, $0100 bytes, at $00:A000 in this driver's image.
;
; Work RAM, reached through the direct page:
;   $20-$5F  a block per voice, eight bytes each: the command (0 none, 1 key-on, 2 key-off, 3 change), the
;            sample's directory entry, the pitch (low byte, high byte), the left volume, the right volume,
;            the play mode (0 continuous, 1 once, 2 repeat), one byte unused
;   $60      the play mailbox: a directory entry to key on voice 0, $FF when none
;   $61      the stop mailbox: non-zero keys every voice off
;   $62      the toggle bit the tick flips on every command, $00 or $10
;   $63      ENDX as the sound program last reported it: a bit per voice at the end of its sample
;   $64      the frames the tick has run, as a byte
;   $65      the voices keyed on this frame, a bit each — the tick's own scratch
;   $66      the voice's bit, the tick's own scratch
;   $70-$7F  a word per voice: a repeat's period, in frames
;   $80-$8F  a word per voice: the frames until a repeat's next strike

        ORG $00:8000
        A8
        X8
init:   LDX #$00
        LDA #$00
clear:  STA $20,X
        INX
        CPX #$70
        BNE clear               ; $20-$8F cleared
        LDA #$FF
        STA $60                 ; no sample waits to play
ready:  LDA !$2140
        CMP #$AA
        BNE ready
        LDA !$2141
        CMP #$BB
        BNE ready
        STZ !$2142              ; destination $0200, low
        LDA #$02
        STA !$2143              ; high
        LDA #$01
        STA !$2141              ; a transfer, not a start
        LDA #$CC
        STA !$2140              ; the kick
kick:   CMP !$2140
        BNE kick                ; acknowledged
        REP #$10                ; 16-bit index registers: 256 bytes to count
        X16
        LDX #$0000
send:   LDA !$A000,X            ; the program's next byte
        STA !$2141
        TXA                     ; its index, the low byte of X
        STA !$2140
echo:   CMP !$2140
        BNE echo
        INX
        CPX #$0100
        BNE send
        SEP #$10
        X8
        STZ !$2142              ; start at $0200, low
        LDA #$02
        STA !$2143
        STZ !$2141              ; zero starts the program
        LDA #$01                ; two past the last index, $FF
        STA !$2140
start:  CMP !$2140
        BNE start               ; the boot program hands over to the sound program
polling: LDA !$2140
        BNE polling             ; the sound program is set up and polling: $00 on port 0
        RTS
