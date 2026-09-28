; The sound program the driver uploads to the audio unit, written for its SPC700. The driver's init sends
; the three pages from $0200 in one transfer — the code, then the sample's directory, the sample and the
; three songs at $0400 — and starts it at $0200.
;
; Voice 0 plays the songs and voice 1 the effect, both on one looping square-wave sample. Timer 0 ticks 32
; times a second; on each tick the program reads a command from the console, steps the song, the effect
; and a fade, and reports what it is doing.
;
; From the console, on the communication ports:
;   port 1   a sequence number: when it changes, port 0 holds a new command
;   port 0   the command — 0, 1 or 2 plays that song from its first note; $10 plays the effect; $20 fades
;            the song out
; To the console:
;   port 0   the song playing, $FF when none
;   port 1   the note playing, 0 to 7
;   port 2   the master volume, $60 at full and falling through a fade
;   port 3   the effect's ticks left, 0 when it is silent
;
; Its own state, in the direct page:
;   $00 the last sequence number seen      $04 the master volume
;   $01 the song, $FF when none            $05 1 while a fade runs
;   $02 the next note                      $06 the effect's ticks left
;   $03 the ticks left on this note        $07 the note playing

        ORG $0200
        MOV $F2,#$5D
        MOV $F3,#$04            ; DIR: the sample directory is page $04
        MOV $F2,#$04
        MOV $F3,#$00            ; V0SRCN: voice 0 plays source 0
        MOV $F2,#$14
        MOV $F3,#$00            ; V1SRCN: voice 1 plays source 0
        MOV $F2,#$00
        MOV $F3,#$40            ; V0VOLL
        MOV $F2,#$01
        MOV $F3,#$40            ; V0VOLR
        MOV $F2,#$10
        MOV $F3,#$30            ; V1VOLL
        MOV $F2,#$11
        MOV $F3,#$30            ; V1VOLR
        MOV $F2,#$07
        MOV $F3,#$7F            ; V0GAIN: direct, full
        MOV $F2,#$17
        MOV $F3,#$00            ; V1GAIN: silent until the effect plays
        MOV $F2,#$6C
        MOV $F3,#$20            ; FLG: unmuted, echo writes off
        MOV $FA,#250            ; T0DIV: timer 0 counts 250 of its 8 kHz ticks, 32 times a second
        MOV $F1,#$01            ; CONTROL: timer 0 on, the boot window unmapped
        MOV A,#$60
        MOV $04,A               ; the master volume, full
        CALL !setvol
        MOV A,#$FF
        MOV $01,A               ; no song
        MOV A,#$00
        MOV $05,A               ; no fade
        MOV $06,A               ; no effect
        MOV $07,A
        MOV A,$F5
        MOV $00,A               ; the sequence number the console left: no command yet

loop:   MOV A,$FD               ; T0OUT: the ticks since the last read, which clears it
        BEQ loop
        CALL !command
        CALL !song
        CALL !effect
        CALL !fade
        CALL !report
        BRA loop

command: MOV A,$F5              ; a new sequence number says port 0 holds a new command
        CMP A,$00
        BEQ done
        MOV $00,A
        MOV A,$F4
        CMP A,#$10
        BEQ blip
        CMP A,#$20
        BEQ fadeon
        CMP A,#$03
        BCS done                ; no song has that number
        MOV $01,A               ; a song, from its first note, at full volume
        MOV A,#$00
        MOV $02,A
        MOV $05,A
        MOV A,#$01
        MOV $03,A               ; its first note on this tick
        MOV A,#$60
        MOV $04,A
        CALL !setvol
done:   RET
blip:   MOV A,#$06
        MOV $06,A               ; six ticks
        MOV $F2,#$17
        MOV $F3,#$7F            ; V1GAIN: full
        MOV $F2,#$12
        MOV $F3,#$00            ; V1PITCHL
        MOV $F2,#$13
        MOV $F3,#$18            ; V1PITCHH: high, falling as the effect runs out
        MOV $F2,#$4C
        MOV $F3,#$02            ; KON: voice 1
        RET
fadeon: MOV A,#$01
        MOV $05,A
        RET

song:   MOV A,$01
        CMP A,#$FF
        BEQ quiet
        DEC $03
        BNE quiet
        MOV A,#$08
        MOV $03,A               ; eight ticks a note: a quarter of a second
        MOV A,$02
        MOV $07,A
        MOV A,$01               ; the note's place in the tables: song x 8 + note
        ASL A
        ASL A
        ASL A
        CLRC
        ADC A,$02
        MOV X,A
        MOV $F2,#$02
        MOV A,!lo+X
        MOV $F3,A               ; V0PITCHL
        MOV $F2,#$03
        MOV A,!hi+X
        MOV $F3,A               ; V0PITCHH
        MOV $F2,#$4C
        MOV $F3,#$01            ; KON: voice 0, each note struck
        MOV A,$02
        INC A
        AND A,#$07              ; eight notes, round and round
        MOV $02,A
quiet:  RET

effect: MOV A,$06
        BEQ still
        DEC $06
        MOV A,$06
        BEQ mute
        ASL A
        ASL A
        MOV $F2,#$13
        MOV $F3,A               ; V1PITCHH: four times the ticks left
        RET
mute:   MOV $F2,#$17
        MOV $F3,#$00            ; V1GAIN: silent
still:  RET

fade:   MOV A,$05
        BEQ held
        MOV A,$04
        SETC
        SBC A,#$02              ; two a tick: a second and a half from full
        BCS lower
        MOV A,#$00
lower:  MOV $04,A
        CALL !setvol
        MOV A,$04
        BNE held
        MOV $05,A               ; faded out: the fade and the song are over
        MOV A,#$FF
        MOV $01,A
held:   RET

setvol: MOV $F2,#$0C
        MOV A,$04
        MOV $F3,A               ; MVOLL
        MOV $F2,#$1C
        MOV $F3,A               ; MVOLR
        RET

report: MOV A,$01
        MOV $F4,A
        MOV A,$07
        MOV $F5,A
        MOV A,$04
        MOV $F6,A
        MOV A,$06
        MOV $F7,A
        RET

        ORG $0400
        DW $0404,$0404          ; directory entry 0: the block right after it, looping on itself
        DB $C3                  ; shift 12, filter 0, loop and end: one block is the whole wave
        DB $77,$77,$77,$77,$99,$99,$99,$99   ; eight samples at +7, eight at -7: a square
; Pitch is the frequency x 16 / 32'000 x 4096, low byte then high, eight notes a song.
lo:     DB $85,$6F,$46,$0A,$46,$6F,$85,$A3   ; 0: A4 C#5 E5 A5 E5 C#5 A4 E4
        DB $30,$B3,$46,$96,$46,$0A,$E7,$5F   ; 1: C5 D5 E5 F5 G5 A5 B5 C6
        DB $C3,$C3,$52,$52,$66,$66,$91,$91   ; 2: A3 A3 E3 E3 F3 F3 G3 G3
hi:     DB $03,$04,$05,$07,$05,$04,$03,$02
        DB $04,$04,$05,$05,$06,$07,$07,$08
        DB $01,$01,$01,$01,$01,$01,$01,$01
