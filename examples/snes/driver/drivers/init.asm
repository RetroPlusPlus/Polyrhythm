; The driver's init, run once when the driver is hosted: it clears the mailbox and sends the sound program
; to the audio unit through the upload protocol the unit's boot program waits in.
;
; The boot program waits with $AA and $BB in ports 0 and 1. The init names the destination, $0200, in
; ports 2 and 3, asks for a transfer with a non-zero port 1, and kicks with $CC in port 0; then it sends
; each byte in port 1 with its index in port 0, waiting for the unit to echo the index back. After the last
; byte it names $0200 again, asks for a start with a zero port 1, and kicks with the last index plus two.
;
; The sound program is 768 bytes at $00:A000 in this driver's image, where its registration places it.

        ORG $00:8000
        A8
        X8
init:   LDA #$FF
        STA $20                 ; the mailbox: $FF says no command waits
        STZ $21                 ; the sequence number the tick sends a command under
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
        REP #$10                ; 16-bit index registers: 768 bytes to count
        X16
        LDX #$0000
send:   LDA !$A000,X            ; the program's next byte
        STA !$2141
        TXA                     ; its index, the low byte of X
        STA !$2140
echo:   CMP !$2140
        BNE echo
        INX
        CPX #$0300
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
        BNE start
        RTS
