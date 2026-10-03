; The default sound driver's program for the SNES's sound CPU: a sample player. The 65816 half of the
; driver uploads it to $0200 through the boot program's transfer protocol and starts it there.
;
; The program sets the S-DSP up once — unmuted, the echo off, the main volume full, every voice with its
; envelope off and its gain direct at full, the sample directory at page $03 — and then polls port 0 for
; commands. A command is one byte: its command in bits 7-5, a toggle in bit 4 the console flips on every
; command so two identical commands are both seen, and the voice in bits 2-0. Its arguments are in ports 1
; to 3, written before it. The program acknowledges a command by echoing its byte on port 0, and
; announces that it is set up and polling by writing $00 there first; the console sends nothing before.
;
;   $20   VOLUME   port 1 the left volume, port 2 the right (VxVOLL, VxVOLR)
;   $40   KEY-ON   port 1 the sample's directory entry, ports 2-3 the pitch, low byte first; the voice's
;                  key-off is cleared and the voice keyed on
;   $60   KEY-OFF  the voice keyed off; its envelope releases
;   $80   PITCH    ports 2-3 the pitch, low byte first, on a voice as it plays
;
; Port 1 carries ENDX to the console on every pass of the loop: a bit per voice that has reached the end of
; its sample.
;
; Its own state, in the direct page:
;   $00 the last command byte seen        $02 the command byte being handled
;   $01 the voices held off (KOF)         $03 the voice's register base, voice x $10

        ORG $0200
        MOV $F1,#$00            ; CONTROL: the boot window unmapped, the timers off
        MOV $F2,#$6C
        MOV $F3,#$20            ; FLG: unmuted, echo writes off
        MOV $F2,#$0C
        MOV $F3,#$7F            ; MVOLL
        MOV $F2,#$1C
        MOV $F3,#$7F            ; MVOLR
        MOV $F2,#$2C
        MOV $F3,#$00            ; EVOLL
        MOV $F2,#$3C
        MOV $F3,#$00            ; EVOLR
        MOV $F2,#$5C
        MOV $F3,#$00            ; KOF: no voice held off
        MOV $F2,#$3D
        MOV $F3,#$00            ; NON
        MOV $F2,#$4D
        MOV $F3,#$00            ; EON
        MOV $F2,#$2D
        MOV $F3,#$00            ; PMON
        MOV $F2,#$5D
        MOV $F3,#$03            ; DIR: the sample directory is page $03
        MOV X,#$00
gains:  MOV A,X
        OR A,#$05
        MOV $F2,A
        MOV $F3,#$00            ; VxADSR1: the envelope off, GAIN decides
        MOV A,X
        OR A,#$07
        MOV $F2,A
        MOV $F3,#$7F            ; VxGAIN: direct, full
        MOV A,X
        CLRC
        ADC A,#$10
        MOV X,A
        CMP X,#$80
        BNE gains               ; eight voices, $00 to $70
        MOV A,$F4
        MOV $00,A               ; what port 0 holds now is not a command
        MOV A,#$00
        MOV $01,A               ; no voice held off
        MOV $F4,A               ; port 0: ready — the console sends nothing before it reads this

loop:   MOV $F2,#$7C
        MOV A,$F3
        MOV $F5,A               ; port 1: ENDX
        MOV A,$F4
        CMP A,$00
        BEQ loop                ; port 0 unchanged: no command
        MOV $00,A
        MOV $02,A
        AND A,#$07
        MOV X,A                 ; the voice
        ASL A
        ASL A
        ASL A
        ASL A
        MOV $03,A               ; its register base
        MOV A,$02
        AND A,#$E0              ; the command
        CMP A,#$20
        BEQ volume
        CMP A,#$40
        BEQ keyon
        CMP A,#$60
        BEQ keyoff
        CMP A,#$80
        BEQ repitch
        BRA ack                 ; a command this program does not have is acknowledged and nothing done

volume: MOV A,$03
        MOV $F2,A
        MOV A,$F5
        MOV $F3,A               ; VxVOLL: port 1
        MOV A,$03
        OR A,#$01
        MOV $F2,A
        MOV A,$F6
        MOV $F3,A               ; VxVOLR: port 2
        BRA ack

keyon:  MOV A,$03
        OR A,#$04
        MOV $F2,A
        MOV A,$F5
        MOV $F3,A               ; VxSRCN: port 1
        CALL !pitch
        MOV A,!bits+X
        EOR A,#$FF
        AND A,$01
        MOV $01,A               ; the voice's hold-off cleared
        MOV $F2,#$5C
        MOV $F3,A               ; KOF
        MOV $F2,#$4C
        MOV A,!bits+X
        MOV $F3,A               ; KON: the voice keyed on
        BRA ack

keyoff: MOV A,!bits+X
        OR A,$01
        MOV $01,A               ; the voice held off
        MOV $F2,#$5C
        MOV $F3,A               ; KOF

repitch: CALL !pitch

ack:    MOV A,$02
        MOV $F4,A               ; port 0: the command byte, acknowledged
        JMP !loop

; The voice's pitch from ports 2 and 3 — a command of its own, and the key-on's second half.
pitch:  MOV A,$03
        OR A,#$02
        MOV $F2,A
        MOV A,$F6
        MOV $F3,A               ; VxPITCHL: port 2
        MOV A,$03
        OR A,#$03
        MOV $F2,A
        MOV A,$F7
        MOV $F3,A               ; VxPITCHH: port 3
        RET

bits:   DB $01,$02,$04,$08,$10,$20,$40,$80

        DS $0300-*              ; the program is one page: the upload sends exactly $0100 bytes
