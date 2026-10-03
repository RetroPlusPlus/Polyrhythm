; The driver's tick, called once a frame. It hands a waiting command to the sound program — the command in
; port 0, then a new sequence number in port 1, which is how the program tells a new command from the one
; it already took — and copies what the program reports into the four bytes the game reads back.
;
; Work RAM, reached through the direct page:
;   $20 the mailbox: a command the game left, $FF when none
;   $21 the sequence number
;   $30 the song playing, $FF when none      $32 the master volume
;   $31 the note playing, 0 to 7             $33 the effect's ticks left
;   $34 the frames this tick has run

        ORG $00:8400
        A8
        X8
tick:   LDA $20
        CMP #$FF
        BEQ report              ; nothing waits
        STA !$2140              ; port 0: the command
        INC $21
        LDA $21
        STA !$2141              ; port 1: a new sequence number
        LDA #$FF
        STA $20                 ; taken
report: LDA !$2140              ; the song
        STA $30
        LDA !$2141              ; the note
        STA $31
        LDA !$2142              ; the master volume
        STA $32
        LDA !$2143              ; the effect's ticks left
        STA $33
        INC $34
        RTS
